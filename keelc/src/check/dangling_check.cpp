// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/dangling_check.h"
#include <algorithm>

namespace keel
{

namespace
{

// `pointer` may hold the address of `target`, and `target` may have ended.
struct Aim
{
    Local_id pointer {};
    Local_id target {};
    bool     ended = false;

    auto operator<=>( const Aim& other ) const = default;
};

using Aims = std::vector<Aim>; // sorted, no duplicates

void normalise( Aims& aims )
{
    std::ranges::sort( aims );
    aims.erase( std::ranges::unique( aims ).begin(), aims.end() );
}

// Set union. Only grows, over finitely many pairs, so the flow ends.
bool merge_into( Aims& into, const Aims& from )
{
    const std::size_t before = into.size();
    into.insert( into.end(), from.begin(), from.end() );
    normalise( into );
    return into.size() != before;
}

// Whole locals whose own address is never taken; `&init` counts, since `reset( out p )` writes `p`
// with no Assign.
std::vector<bool> tracked_pointers( const Function& func )
{
    std::vector<bool> tracked( func.locals.size(), true );

    for( const Statement& statement : func.statements )
    {
        const Place& source = statement.value.a.place;

        if( statement.kind == Statement_kind::Assign && statement.value.kind == Rvalue_kind::Address_of &&
            !source.is_global() && !has_deref_projection( func, source ) )
        {
            tracked[source.local.v] = false;
        }
    }

    return tracked;
}

struct Walk
{
    const Function&              func;
    const std::vector<bool>&     tracked;
    std::vector<Dangling_error>* errors   = nullptr; // null while finding the fixpoint
    std::vector<bool>*           reported = nullptr;

    // A place starting `(*p)` reads through `p`.
    void read( const Aims& aims, const Place& place, Span at ) const
    {
        if( errors == nullptr || place.is_global() || place.num_projections == 0 ||
            func.projections[place.first_projection].kind != Projection_kind::Deref || ( *reported )[place.local.v] )
        {
            return;
        }

        for( const Aim& aim : aims )
        {
            if( aim.pointer == place.local && aim.ended )
            {
                ( *reported )[place.local.v] = true;
                errors->push_back( Dangling_error { .pointer = aim.pointer, .ended = aim.target, .at = at } );
                return;
            }
        }
    }

    void read_operand( const Aims& aims, const Operand& operand, Span at ) const
    {
        if( operand.has_place() )
        {
            read( aims, operand.place, at );
        }
    }

    void assign( Aims& aims, const Statement& statement ) const
    {
        const Place&  place = statement.place;
        const Rvalue& value = statement.value;

        if( place.is_global() || place.num_projections != 0 || !tracked[place.local.v] )
        {
            return;
        }

        // Gathered before the old aims go, for `q = copy q`.
        Aims gained;

        if( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() &&
            !has_deref_projection( func, value.a.place ) )
        {
            gained.push_back( Aim { .pointer = place.local, .target = value.a.place.local } );
        }
        else if( ( value.kind == Rvalue_kind::Use || value.kind == Rvalue_kind::Cast ) && value.a.has_place() &&
                 !value.a.place.is_global() && value.a.place.num_projections == 0 )
        {
            for( const Aim& aim : aims )
            {
                if( aim.pointer == value.a.place.local )
                {
                    gained.push_back( Aim { .pointer = place.local, .target = aim.target, .ended = aim.ended } );
                }
            }
        }

        std::erase_if( aims, [&]( const Aim& aim ) { return aim.pointer == place.local; } );
        aims.insert( aims.end(), gained.begin(), gained.end() );
        normalise( aims );
    }

    void end( Aims& aims, Local_id local ) const
    {
        std::erase_if( aims, [&]( const Aim& aim ) { return aim.pointer == local; } );

        for( Aim& aim : aims )
        {
            aim.ended = aim.ended || aim.target == local;
        }

        normalise( aims );
    }

    void block( u32 index, Aims& aims ) const
    {
        const Block& block = func.blocks[index];

        for( u32 i = 0; i < block.statement_count; ++i )
        {
            const Statement& statement = func.statements[block.first_statement + i];

            switch( statement.kind )
            {
            case Statement_kind::Assign:
                read( aims, statement.place, statement.span );
                for_each_operand(
                    func, statement.value, [&]( const Operand& operand ) { read_operand( aims, operand, statement.span ); }
                );
                if( statement.value.kind == Rvalue_kind::Address_of )
                {
                    read( aims, statement.value.a.place, statement.span );
                }
                assign( aims, statement );
                break;
            case Statement_kind::Drop:
                read( aims, statement.place, statement.span );
                break;
            case Statement_kind::Storage_live:
                break;
            case Statement_kind::Storage_dead:
                if( !statement.place.is_global() )
                {
                    end( aims, statement.place.local );
                }
                break;
            }
        }

        const Terminator& terminator = block.terminator;
        read_operand( aims, terminator.condition, terminator.span );
        read_operand( aims, terminator.message, terminator.span );
        read_operand( aims, terminator.at, terminator.span );
    }
};

} // namespace

std::vector<Dangling_error> check_dangling( const Function& func )
{
    const std::vector<bool> tracked = tracked_pointers( func );
    std::vector<Aims>       in( func.blocks.size() );
    std::vector<bool>       seen( func.blocks.size(), false );

    std::vector<Block_id> work { Block_id { 0 } };
    std::vector<Block_id> next;

    const Walk flow { .func = func, .tracked = tracked };

    while( !work.empty() )
    {
        const Block_id block = work.back();
        work.pop_back();

        Aims out = in[block.v];
        flow.block( block.v, out );

        next.clear();
        successors( func.blocks[block.v].terminator, next );

        // An empty state merges as no change, so a first visit is pushed regardless.
        for( const Block_id target : next )
        {
            if( merge_into( in[target.v], out ) || !seen[target.v] )
            {
                seen[target.v] = true;
                work.push_back( target );
            }
        }
    }

    // A second walk, each block once, so a block on a back edge reports once.
    std::vector<Dangling_error> errors;
    std::vector<bool>           reported( func.locals.size(), false );
    const Walk                  report { .func = func, .tracked = tracked, .errors = &errors, .reported = &reported };

    for( u32 block = 0; block < func.blocks.size(); ++block )
    {
        report.block( block, in[block] );
    }

    return errors;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

// `f`'s errors, with the pointer and the ended local named.
struct Dangled : Compiled
{
    explicit Dangled( std::string_view body )
        : Compiled(
              "void reset( out i32* q ) { q = nullptr; }\nvoid look( i32* q ) { }\ni32 f() { " + std::string( body ) +
              " }\ni32 main() { return 0; }"
          )
    {
        for( const Function& function : functions )
        {
            if( function.declaration.is_valid() && interner.text( ast.name( function.declaration ) ) == "f" )
            {
                tested = &function;
            }
        }
    }

    std::vector<Dangling_error> errors() const
    {
        return check_dangling( *tested );
    }

    std::string_view name_of( Local_id local ) const
    {
        return interner.text( tested->locals[local.v].name );
    }

    const Function* tested = nullptr;
};

} // namespace

TEST_CASE( "dangling_check_refuses_a_read_after_the_target_ended", "[check][dangling]" )
{
    for( const char* body : {
             "i32* p = nullptr; { i32 inner = 1; p = &inner; } return *p;",
             "i32* p = nullptr; i32 n = 0; while( n < 2 ) { i32 inner = n; p = &inner; n++; } return *p;",
             "i32* p = nullptr; { i32 inner = 1; i32* q = &inner; p = q; } return *p;",
         } )
    {
        const Dangled p( body );

        INFO( body << "\n" << p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.tested != nullptr );

        const std::vector<Dangling_error> errors = p.errors();

        REQUIRE( errors.size() == 1 );
        REQUIRE( p.name_of( errors[0].pointer ) == "p" );
        REQUIRE( p.name_of( errors[0].ended ) == "inner" );
        REQUIRE( p.sm.text( errors[0].at ).find( "*p" ) != std::string_view::npos );
    }
}

// Read while the target lives, aimed elsewhere first, written by an `out` the pass cannot follow,
// or only passed on.
TEST_CASE( "dangling_check_accepts_a_pointer_not_read_after_its_target", "[check][dangling]" )
{
    for( const char* body : {
             "i32* p = nullptr; i32 r = 0; { i32 inner = 1; p = &inner; r = *p; } return r;",
             "i32 outer = 2; i32* p = nullptr; { i32 inner = 1; p = &inner; } p = &outer; return *p;",
             "i32* p = nullptr; { i32 inner = 1; p = &inner; } reset( out p ); return 0;",
             "i32* p = nullptr; { i32 inner = 1; p = &inner; } look( p ); return 0;",
         } )
    {
        const Dangled p( body );

        INFO( body << "\n" << p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.tested != nullptr );
        REQUIRE( p.errors().empty() );
    }
}

} // namespace keel
#endif
