// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/coalesce_moves.h"
#include <algorithm>
#include <optional>
#include "ir/initialised.h"

namespace keel
{
namespace
{

struct Candidate
{
    u32      statement;
    Local_id kept;
    Local_id gone;
};

bool whole_local( const Place& place )
{
    return !place.is_global() && place.num_projections == 0;
}

bool is_parameter( const Function& func, Local_id local )
{
    return local.v >= 1 && local.v <= func.parameter_count;
}

bool is_fixed( const Function& func, Local_id local )
{
    return local == k_return_slot || is_parameter( func, local );
}

bool names( const Place& place, Local_id local )
{
    return !place.is_global() && place.local == local;
}

u32 reads_of( const Function& func, Local_id local )
{
    u32 reads = 0;

    for( const Statement& statement : func.statements )
    {
        for_each_operand(
            func,
            statement.value,
            [&]( const Operand& operand )
            {
                if( operand.has_place() && names( operand.place, local ) )
                {
                    ++reads;
                }
            }
        );

        if( statement.kind == Statement_kind::Assign && statement.value.kind == Rvalue_kind::Address_of &&
            statement.value.address_purpose != Address_purpose::Initialise && names( statement.value.a.place, local ) )
        {
            ++reads;
        }

        if( statement.kind == Statement_kind::Drop && names( statement.place, local ) )
        {
            ++reads;
        }

        if( statement.kind == Statement_kind::Assign && has_deref_projection( func, statement.place ) &&
            names( statement.place, local ) )
        {
            ++reads;
        }
    }

    for( const Block& block : func.blocks )
    {
        const Terminator& terminator = block.terminator;
        if( terminator.condition.has_place() && names( terminator.condition.place, local ) )
        {
            ++reads;
        }

        if( terminator.message.has_place() && names( terminator.message.place, local ) )
        {
            ++reads;
        }

        if( terminator.at.has_place() && names( terminator.at.place, local ) )
        {
            ++reads;
        }
    }

    for( const Projection& projection : func.projections )
    {
        if( projection.kind == Projection_kind::Index && projection.index == local )
        {
            ++reads;
        }
    }

    return reads;
}

std::optional<Candidate> candidate_at( const Function& func, u32 index )
{
    const Statement& statement   = func.statements[index];
    const Place&     source      = statement.value.a.place;
    const Place&     destination = statement.place;

    if( statement.kind != Statement_kind::Assign || !whole_local( destination ) || statement.value.kind != Rvalue_kind::Use ||
        !( statement.value.a.kind == Operand_kind::Move || statement.value.a.kind == Operand_kind::Copy ) ||
        !whole_local( source ) || destination.local == source.local ||
        func.locals[destination.local.v].type != func.locals[source.local.v].type )
    {
        return {};
    }

    Place kept;
    Place gone;
    if( is_fixed( func, source.local ) && !is_fixed( func, destination.local ) )
    {
        kept = source;
        gone = destination;
    }
    else if( !is_fixed( func, source.local ) )
    {
        kept = destination;
        gone = source;
    }
    else
    {
        return {};
    }

    if( std::ranges::find( func.one_path_temporaries, gone.local ) != func.one_path_temporaries.end() &&
        is_parameter( func, kept.local ) )
    {
        return {};
    }

    // A copy is a move in all but name only at its source's last read, of a source nothing drops.
    if( statement.value.a.kind == Operand_kind::Copy )
    {
        const bool dropped = std::ranges::any_of(
            func.statements,
            [&]( const Statement& other ) { return other.kind == Statement_kind::Drop && names( other.place, source.local ); }
        );

        if( dropped || reads_of( func, source.local ) > 1 )
        {
            return {};
        }
    }

    return Candidate { index, kept.local, gone.local };
}

bool interferes( const Function& func, Local_id kept, Local_id gone, const std::vector<Occupancy>& entry )
{
    const auto clash = [&]( const Occupancy& s )
    { return s.initialised[gone.v] && ( s.initialised[kept.v] || s.storage_dead[kept.v] ); };

    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        const Block& block = func.blocks[b];
        Occupancy    state = entry[b];

        if( clash( state ) )
        {
            return true;
        }

        for( u32 i = 0; i < block.statement_count; ++i )
        {
            const Statement& statement = func.statements[block.first_statement + i];
            step( func, statement, state );

            if( clash( state ) )
            {
                return true;
            }
        }
    }

    return false;
}

void rename( Function& func, Local_id gone, Local_id kept )
{
    for( Statement& statement : func.statements )
    {
        if( names( statement.place, gone ) )
        {
            statement.place.local = kept;
        }

        if( names( statement.value.a.place, gone ) )
        {
            statement.value.a.place.local = kept;
        }

        if( names( statement.value.b.place, gone ) )
        {
            statement.value.b.place.local = kept;
        }
    }

    for( Operand& operand : func.operands )
    {
        if( operand.has_place() && names( operand.place, gone ) )
        {
            operand.place.local = kept;
        }
    }

    for( Block& block : func.blocks )
    {
        Terminator& terminator = block.terminator;

        if( terminator.condition.has_place() && names( terminator.condition.place, gone ) )
        {
            terminator.condition.place.local = kept;
        }

        if( terminator.message.has_place() && names( terminator.message.place, gone ) )
        {
            terminator.message.place.local = kept;
        }

        if( terminator.at.has_place() && names( terminator.at.place, gone ) )
        {
            terminator.at.place.local = kept;
        }
    }

    for( Projection& projection : func.projections )
    {
        if( projection.kind == Projection_kind::Index && projection.index == gone )
        {
            projection.index = kept;
        }
    }
}

bool coalesce_one( Function& func )
{
    std::vector<Occupancy>   entry = occupancy_on_entry( func );
    std::optional<Candidate> chosen;
    for( u32 index = 0; index < func.statements.size(); ++index )
    {
        const std::optional<Candidate> candidate = candidate_at( func, index );
        if( !candidate.has_value() )
        {
            continue;
        }

        Operand&   source   = func.statements[index].value.a;
        const bool was_copy = source.kind == Operand_kind::Copy;

        if( was_copy )
        {
            source.kind = Operand_kind::Move;
            entry       = occupancy_on_entry( func );
        }

        if( !interferes( func, candidate->kept, candidate->gone, entry ) )
        {
            chosen = candidate;
            break;
        }

        if( was_copy )
        {
            source.kind = Operand_kind::Copy;
            entry       = occupancy_on_entry( func );
        }
    }

    if( !chosen )
    {
        return false;
    }

    std::vector<bool> deleted( func.statements.size(), false );
    bool              gone_drop_kept = false;
    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        const Block& block = func.blocks[b];
        Occupancy    state = entry[b];

        for( u32 i = 0; i < block.statement_count; ++i )
        {
            const u32        at        = block.first_statement + i;
            const Statement& statement = func.statements[at];

            if( at == chosen->statement )
            {
                deleted[at] = true;
            }
            else if( ( statement.kind == Statement_kind::Storage_dead || statement.kind == Statement_kind::Storage_live ) &&
                     names( statement.place, chosen->gone ) )
            {
                deleted[at] = true;
            }
            else if( statement.kind == Statement_kind::Drop )
            {
                if( ( names( statement.place, chosen->gone ) && !state.initialised[chosen->gone.v] ) ||
                    ( names( statement.place, chosen->kept ) && !state.initialised[chosen->kept.v] ) )
                {
                    deleted[at] = true;
                }
                else if( names( statement.place, chosen->gone ) )
                {
                    gone_drop_kept = true;
                }
            }

            step( func, statement, state );
        }
    }

    rename( func, chosen->gone, chosen->kept );

    std::vector<Statement> statements;
    statements.reserve( func.statements.size() );

    for( Block& block : func.blocks )
    {
        const u32 old_first   = block.first_statement;
        const u32 old_count   = block.statement_count;
        block.first_statement = narrow_cast<u32>( statements.size() );

        u32 kept_count = 0;
        for( u32 i = 0; i < old_count; ++i )
        {
            if( !deleted[old_first + i] )
            {
                statements.push_back( func.statements[old_first + i] );
                ++kept_count;
            }
        }

        block.statement_count = kept_count;
    }

    func.statements = std::move( statements );

    std::erase( func.one_path_temporaries, chosen->gone );

    if( gone_drop_kept && std::ranges::find( func.one_path_temporaries, chosen->kept ) == func.one_path_temporaries.end() )
    {
        func.one_path_temporaries.push_back( chosen->kept );
    }

    return true;
}

} // namespace

void coalesce_moves( Function& func )
{
    while( coalesce_one( func ) )
    {
        // Keep going until no more candidates.
    }
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

constexpr std::string_view k_types = "class Box { public i32 v; public Box( i32 n ) { v = n; } ~Box() { } };\n"
                                     "struct Point { i32 x; i32 y; };\n"
                                     "class Inner { i32 n; public Inner() { n = 0; } ~Inner() { } };\n"
                                     "class Outer { Inner inner; public Outer() { inner = Inner(); } ~Outer() { } };\n"
                                     "Box made( i32 v ) { Box b = Box( v ); return b; }\n"
                                     "Outer outer() { return Outer(); }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Optimised : Compiled
{
    explicit Optimised( std::string_view source )
        : Compiled( std::string( source ) + std::string( k_types ), Stage::Elaborated, Build { .optimise = true } )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    // Whole locals copied or moved into whole locals: what the pass exists to remove.
    std::size_t local_copies() const
    {
        return std::ranges::count_if(
            first().statements,
            []( const Statement& s )
            {
                return s.kind == Statement_kind::Assign && s.value.kind == Rvalue_kind::Use && s.value.a.has_place() &&
                       whole_local( s.value.a.place ) && whole_local( s.place );
            }
        );
    }

    std::size_t drops_of( u32 local, bool projected ) const
    {
        return std::ranges::count_if(
            first().statements,
            [&]( const Statement& s )
            {
                return s.kind == Statement_kind::Drop && names( s.place, Local_id { local } ) &&
                       ( s.place.num_projections != 0 ) == projected;
            }
        );
    }

    bool well_formed() const
    {
        return std::ranges::all_of( all, []( const Function& f ) { return verify( f ).empty(); } );
    }
};

} // namespace

TEST_CASE( "coalesce_moves_builds_in_place", "[opt][coalesce]" )
{
    SECTION( "a local returned is built in the return slot, with no flag" )
    {
        const Optimised p( "Box f( i32 v ) { Box b = Box( v ); return b; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 0 );
        REQUIRE( std::ranges::none_of( p.first().statements, []( const Statement& s ) { return s.drop_flag.is_valid(); } ) );
    }

    SECTION( "a call's result into a local" )
    {
        const Optimised p( "i32 f() { Box b = made( 1 ); return b.v; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 0 );
    }

    SECTION( "a struct literal, through a copy at its last read" )
    {
        const Optimised p( "Point f( i32 v ) { Point p = Point { v, v }; return p; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 0 );
    }

    SECTION( "each arm of a conditional" )
    {
        const Optimised p( "Box f( bool c ) { Box r = c ? Box( 1 ) : Box( 2 ); return r; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 0 );
        REQUIRE(
            std::ranges::none_of( p.first().statements, []( const Statement& s ) { return s.kind == Statement_kind::Drop; } )
        );
    }
}

TEST_CASE( "coalesce_moves_keeps_two_values_alive_at_once", "[opt][coalesce]" )
{
    SECTION( "a reassignment builds the new value before the old one drops" )
    {
        const Optimised p( "i32 f() { Box a = Box( 1 ); a = Box( 2 ); return a.v; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 1 );
    }

    SECTION( "a local alive while an early return builds its own value" )
    {
        const Optimised p( "Box f( bool c ) { Box b = Box( 3 ); if( c ) { return Box( 4 ); } return b; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 1 );
    }

    SECTION( "a copy read again" )
    {
        const Optimised p( "i32 f() { Point p = Point { 1, 2 }; Point q = p; p.x = 5; return q.x; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 1 );
    }
}

// The scrutinee merges into the parameter; the arm that never moved it drops it once, and the
// parameter's own drop, of a value already moved, goes.
TEST_CASE( "coalesce_moves_drops_a_scrutinee_once", "[opt][coalesce]" )
{
    const Optimised p(
        "i32 f( move Shape s ) { switch( move s ) { case Shape::boxed( b ): return b.v; case Shape::empty: return 0; } }\n"
        "enum Shape { boxed( Box b ), empty };\n"
    );
    REQUIRE( p.clean() );
    REQUIRE( p.well_formed() );
    REQUIRE( p.drops_of( 1, false ) == 1 );
}

// `drop _1` runs `~Outer`'s body and `drop _1.inner` the field after it; neither may go.
TEST_CASE( "coalesce_moves_keeps_the_field_drops_after_a_destructor", "[opt][coalesce]" )
{
    const Optimised p( "i32 f() { Outer o = outer(); return 0; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( p.well_formed() );
    REQUIRE( p.local_copies() == 0 );
    REQUIRE( p.drops_of( 1, false ) == 1 );
    REQUIRE( p.drops_of( 1, true ) == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
