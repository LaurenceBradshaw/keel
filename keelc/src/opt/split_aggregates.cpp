// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/split_aggregates.h"
#include <algorithm>
#include <map>
#include "ir/live.h"
#include "ir/place_type.h"
#include "ir/simplify.h"

namespace keel
{

namespace
{
// `_P = &init _L`, building the whole of `_L`.
bool initialises_whole( const Statement& statement )
{
    return statement.kind == Statement_kind::Assign && statement.value.kind == Rvalue_kind::Address_of &&
           statement.value.address_purpose == Address_purpose::Initialise && !statement.value.a.place.is_global() &&
           statement.value.a.place.num_projections == 0;
}

// The locals to split. A local's storage markers don't count, nor does an `&init` of it whose pointer
// nothing else names: an inlined constructor's, left after its body.
Local_set splittable( const Function& func, const Type_table& table )
{
    Local_set        split( func.locals.size(), false );
    std::vector<u32> mentions( func.locals.size(), 0 );
    std::vector<u32> inits;

    for( u32 l = func.parameter_count + 1; l < func.locals.size(); ++l )
    {
        split[l] = table.is_struct( func.locals[l].type );
    }

    const auto use = [&]( const Place& place )
    {
        if( place.is_global() || !place.local.is_valid() )
        {
            return;
        }

        mentions[place.local.v] += 1;

        if( place.num_projections == 0 || func.projections[place.first_projection].kind != Projection_kind::Field )
        {
            split[place.local.v] = false;
        }
    };

    for( u32 s = 0; s < func.statements.size(); ++s )
    {
        const Statement& statement = func.statements[s];

        if( statement.kind == Statement_kind::Storage_dead || statement.kind == Statement_kind::Storage_live )
        {
            continue;
        }

        if( initialises_whole( statement ) )
        {
            use( statement.place );
            inits.push_back( s );
            continue;
        }

        use( statement.place );

        if( statement.kind == Statement_kind::Assign )
        {
            for_each_operand(
                func,
                statement.value,
                [&]( const Operand& operand )
                {
                    if( operand.has_place() )
                    {
                        use( operand.place );
                    }
                }
            );

            if( statement.value.kind == Rvalue_kind::Address_of )
            {
                use( statement.value.a.place );
            }
        }
    }

    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        const Terminator& terminator = func.blocks[b].terminator;

        if( terminator.condition.has_place() )
        {
            use( terminator.condition.place );
        }

        if( terminator.message.has_place() )
        {
            use( terminator.message.place );
        }

        if( terminator.at.has_place() )
        {
            use( terminator.at.place );
        }
    }

    for( u32 s : inits )
    {
        const Statement& statement = func.statements[s];
        const Local_id   built     = statement.value.a.place.local;

        if( statement.place.is_global() || statement.place.num_projections != 0 || mentions[statement.place.local.v] != 1 )
        {
            split[built.v] = false;
        }
    }

    for( u32 l = 0; l < func.locals.size(); ++l )
    {
        split[l] = split[l] && mentions[l] > 0;
    }

    return split;
}

// One new local per split local and field named, keyed by both; each place loses its first
// projection, and the split locals' markers and `&init`s go.
void split_locals( Function& func, const Local_set& split, const Ast& ast, Types& types )
{
    std::map<std::pair<u32, u32>, Local_id> parts;
    std::vector<bool>                       deleted( func.statements.size(), false );

    const auto rewrite = [&]( Place& place )
    {
        if( place.is_global() || !place.local.is_valid() || !split[place.local.v] )
        {
            return;
        }

        const Node_id field = func.projections[place.first_projection].field;

        if( !parts.contains( { place.local.v, field.v } ) )
        {
            const Local   whole = func.locals[place.local.v];
            const Type_id type  = place_type(
                ast,
                types,
                func,
                Place { .local = place.local, .first_projection = place.first_projection, .num_projections = 1 }
            );
            parts[{ place.local.v, field.v }] = Local_id { narrow_cast<u32>( func.locals.size() ) };
            func.locals.push_back( Local { .type = type, .span = whole.span, .name = whole.name } );
        }

        place.local = parts[{ place.local.v, field.v }];
        place.first_projection += 1;
        place.num_projections -= 1;
    };

    for( u32 s = 0; s < func.statements.size(); ++s )
    {
        Statement& statement = func.statements[s];

        if( statement.kind == Statement_kind::Storage_dead || statement.kind == Statement_kind::Storage_live )
        {
            deleted[s] = split[statement.place.local.v];
            continue;
        }

        if( initialises_whole( statement ) && split[statement.value.a.place.local.v] )
        {
            deleted[s] = true;
            continue;
        }

        rewrite( statement.place );
        rewrite( statement.value.a.place );
        rewrite( statement.value.b.place );

        for( u32 i = 0; i < statement.value.argument_count; ++i )
        {
            rewrite( func.operands[statement.value.first_argument + i].place );
        }
    }

    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        Terminator& terminator = func.blocks[b].terminator;

        rewrite( terminator.condition.place );
        rewrite( terminator.message.place );
        rewrite( terminator.at.place );
    }

    remove_statements( func, deleted );
}

} // namespace

void split_aggregates( Function& func, const Ast& ast, Types& types )
{
    // A field's new local may be a struct named only through its own fields in turn.
    while( true )
    {
        const Local_set split = splittable( func, types.table() );

        if( std::ranges::none_of( split, []( bool s ) { return s; } ) )
        {
            return;
        }

        split_locals( func, split, ast, types );
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

constexpr std::string_view k_types = "struct Point { i32 x; i32 y; };\n"
                                     "enum Shape { dot( Point at ), none };\n"
                                     "class Box { public Box( i32 v ) { value = v; } public i32 value; };\n"
                                     "class Held { public Held( i32 v ) { box = Box( v ); n = v + 1; } "
                                     "public Box box; public i32 n; };\n";

// The release pipeline with splitting off, then the pass over the function declaring `name`.
struct Split : Compiled
{
    Function func;
    Local_id local {};

    explicit Split( std::string_view source, std::string_view name, std::optional<Inline_weights> inlining = Inline_weights {} )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build { .optimise = true, .inlining = inlining, .splitting = false }
          )
    {
        for( const Function& f : functions )
        {
            for( u32 l = 0; l < f.locals.size() && !local.is_valid(); ++l )
            {
                if( f.locals[l].name.is_valid() && interner.text( f.locals[l].name ) == name )
                {
                    func  = f;
                    local = Local_id { l };
                }
            }
        }

        split_aggregates( func, ast, types );
    }

    // Whether anything but a storage marker still names the local.
    bool named() const
    {
        const auto names = [&]( const Place& place ) { return !place.is_global() && place.local == local; };
        const auto reads = [&]( const Operand& operand ) { return operand.has_place() && names( operand.place ); };

        for( const Statement& s : func.statements )
        {
            if( s.kind == Statement_kind::Storage_live || s.kind == Statement_kind::Storage_dead )
            {
                continue;
            }

            bool any = names( s.place ) || ( s.value.kind == Rvalue_kind::Address_of && names( s.value.a.place ) );
            for_each_operand( func, s.value, [&]( const Operand& operand ) { any = any || reads( operand ); } );

            if( any )
            {
                return true;
            }
        }

        return std::ranges::any_of( func.blocks, [&]( const Block& b ) { return reads( b.terminator.condition ); } );
    }

    std::size_t struct_locals_named() const
    {
        std::size_t count = 0;
        for( const Statement& s : func.statements )
        {
            count += s.kind == Statement_kind::Assign && !s.place.is_global() &&
                     types.table().is_struct( func.locals[s.place.local.v].type );
        }
        return count;
    }

    bool initialises() const
    {
        return std::ranges::any_of(
            func.statements,
            []( const Statement& s )
            { return s.value.kind == Rvalue_kind::Address_of && s.value.address_purpose == Address_purpose::Initialise; }
        );
    }
};

} // namespace

TEST_CASE( "split_aggregates_splits_a_constructed_local", "[opt][split]" )
{
    const Split p( "i32 f() { Box b = Box( 4 ); return b.value; }\n", "b" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE_FALSE( p.named() );
    REQUIRE_FALSE( p.initialises() );
    REQUIRE( p.struct_locals_named() == 0 );
}

// `h` splits into `box` and `n` on the first round, and `box` into `value` on the second.
TEST_CASE( "split_aggregates_splits_a_nested_field", "[opt][split]" )
{
    const Split p( "i32 f() { Held h = Held( 5 ); return h.box.value + h.n; }\n", "h" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE_FALSE( p.named() );
    REQUIRE( p.struct_locals_named() == 0 );
}

TEST_CASE( "split_aggregates_keeps_an_addressed_local", "[opt][split]" )
{
    const Split p( "i32 f( i32 a ) { Point p = Point { a, 1 }; ref Point r = p; r.x = 3; return p.x; }\n", "p" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.named() );
}

TEST_CASE( "split_aggregates_keeps_a_whole_copy", "[opt][split]" )
{
    const Split p( "i32 f( i32 a ) { Point p = Point { a, 1 }; Point q = p; q.x = 7; return p.x + q.x; }\n", "p" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.named() );
}

TEST_CASE( "split_aggregates_keeps_an_enum", "[opt][split]" )
{
    const Split p(
        "i32 f( i32 a ) { Shape s = Shape::dot( Point { a, 2 } ); "
        "switch( s ) { case Shape::dot( at ): return at.x; case Shape::none: return 0; } }\n",
        "s"
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.named() );
}

// Not inlined, so the constructor's call reads the `&init` pointer and `b` stays whole.
TEST_CASE( "split_aggregates_keeps_a_local_a_constructor_still_builds", "[opt][split]" )
{
    const Split p( "i32 f() { Box b = Box( 4 ); return b.value; }\n", "b", std::nullopt );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.func ).empty() );
    REQUIRE( p.initialises() );
    REQUIRE( p.named() );
}

} // namespace keel

#endif
