// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/recognise_rotates.h"

namespace keel
{
namespace
{

// The shift `operand` holds at `index` of `block`, written there by `x << c` or `x >> c` of a whole
// local `x` and a constant `c`.
struct Shift
{
    u32        at = 0;
    Token_kind op {};
    Local_id   x {};
    u64        by = 0;
};

std::optional<Shift>
shift_of( const Function& func, const Block& block, u32 index, const Operand& operand, const Literal_pool& literals )
{
    const std::optional<u32> s = last_write( func, block, index, operand );
    if( !s )
    {
        return std::nullopt;
    }

    const Rvalue& value = func.rvalue( func.statements_of( block )[*s] );
    if( value.kind != Rvalue_kind::Binary || ( value.op != Token_kind::Less_less && value.op != Token_kind::Greater_greater ) ||
        value.a.kind != Operand_kind::Copy || !value.a.place.is_whole_local() || value.b.kind != Operand_kind::Constant ||
        literals.kind( value.b.constant ) != Literal_kind::Integer )
    {
        return std::nullopt;
    }

    return Shift { .at = *s, .op = value.op, .x = value.a.place.local, .by = literals.integer( value.b.constant ) };
}

} // namespace

bool recognise_rotates( Function& func, const Type_table& table, const Literal_pool& literals )
{
    // A store through a pointer could change an addressed `x` between its shifts and the `|`.
    std::vector<bool> addressed( func.locals.size(), false );
    for( const Statement& statement : func.statements )
    {
        if( statement.value.is_valid() && func.rvalue( statement ).kind == Rvalue_kind::Address_of &&
            !func.rvalue( statement ).a.place.is_global() )
        {
            addressed[func.rvalue( statement ).a.place.local.v] = true;
        }
    }

    bool changed = false;
    for( const Block& block : func.blocks )
    {
        const Run<Statement> statements = func.statements_of( block );
        for( u32 s = 0; s < statements.size(); ++s )
        {
            const Statement& statement = statements[s];
            if( statement.kind != Statement_kind::Assign || !statement.value.is_valid() )
            {
                continue;
            }

            const Rvalue& value = func.rvalue( statement );
            if( value.kind != Rvalue_kind::Binary || value.op != Token_kind::Pipe )
            {
                continue;
            }
            const Type& type = table.get( value.type );
            if( type.kind != Type_kind::Int || type.is_signed )
            {
                continue;
            }

            const std::optional<Shift> a = shift_of( func, block, s, value.a, literals );
            const std::optional<Shift> b = shift_of( func, block, s, value.b, literals );
            if( !a || !b || a->x != b->x || a->op == b->op || addressed[a->x.v] || func.locals[a->x].type != value.type )
            {
                continue;
            }

            const Shift& left  = a->op == Token_kind::Less_less ? *a : *b;
            const Shift& right = a->op == Token_kind::Less_less ? *b : *a;
            if( left.by == 0 || left.by >= type.width || left.by + right.by != type.width )
            {
                continue;
            }

            // `x` as both shifts read it.
            bool written = false;
            for( u32 between = std::min( a->at, b->at ) + 1; between < s && !written; ++between )
            {
                const Statement& other = statements[between];
                written = other.kind == Statement_kind::Assign && !other.place.is_global() && other.place.local == left.x;
            }
            if( written )
            {
                continue;
            }

            const Rvalue& shifted    = func.rvalue( statements[left.at] );
            func.rvalue( statement ) = rotate_left( copy( Place { .local = left.x }, value.type ), shifted.b );
            changed                  = true;
        }
    }

    return changed;
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

// Rotates left in the first function, optimised and elaborated.
std::size_t rotates( std::string_view source, u32 level = 2 )
{
    const Compiled c( std::string( source ) + "i32 main() { return 0; }\n", Stage::Elaborated, Build { .level = level } );
    INFO( c.rendered() );
    REQUIRE( c.clean() );
    const Function& func = c.functions.front();
    REQUIRE( verify( func ).empty() );
    std::size_t count = 0;
    for( const Statement& statement : func.statements )
    {
        count += statement.value.is_valid() && func.rvalue( statement ).kind == Rvalue_kind::Rotate;
    }
    return count;
}

} // namespace

TEST_CASE( "recognise_rotates_makes_a_shift_or_a_rotate", "[opt][recognise_rotates]" )
{
    REQUIRE( rotates( "u64 f( u64 x ) { return ( x << 13 ) | ( x >> 51 ); }\n" ) == 1 );
    REQUIRE( rotates( "u32 f( u32 x ) { return ( x >> 25 ) | ( x << 7 ); }\n" ) == 1 );
    REQUIRE( rotates( "u8 f( u8 x ) { return ( x << 3 ) | ( x >> 5 ); }\n" ) == 1 );
}

TEST_CASE( "recognise_rotates_takes_a_helper_once_inlined", "[opt][recognise_rotates]" )
{
    REQUIRE(
        rotates( "u64 f( u64 x ) { return spin( x, 21 ); }\n"
                 "u64 spin( u64 x, u64 by ) { return ( x << by ) | ( x >> ( 64 - by ) ); }\n" ) == 1
    );
}

TEST_CASE( "recognise_rotates_leaves_what_is_not_one", "[opt][recognise_rotates]" )
{
    // A signed `>>` copies the sign.
    REQUIRE( rotates( "i64 f( i64 x ) { return ( x << 13 ) | ( x >> 51 ); }\n" ) == 0 );
    // The amounts do not make the width.
    REQUIRE( rotates( "u64 f( u64 x ) { return ( x << 13 ) | ( x >> 50 ); }\n" ) == 0 );
    // Two values.
    REQUIRE( rotates( "u64 f( u64 x, u64 y ) { return ( x << 13 ) | ( y >> 51 ); }\n" ) == 0 );
    // `x` changes between the shifts.
    REQUIRE( rotates( "u64 f( u64 x ) { u64 a = x << 13; x += 1; u64 b = x >> 51; return a | b; }\n" ) == 0 );
    // A variable amount.
    REQUIRE( rotates( "u64 f( u64 x, u64 by ) { return ( x << by ) | ( x >> ( 64 - by ) ); }\n" ) == 0 );
}

TEST_CASE( "recognise_rotates_runs_from_O2", "[opt][recognise_rotates]" )
{
    REQUIRE( rotates( "u64 f( u64 x ) { return ( x << 13 ) | ( x >> 51 ); }\n", 1 ) == 0 );
}

} // namespace keel
#endif
