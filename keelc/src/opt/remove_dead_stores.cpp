// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/remove_dead_stores.h"
#include "ir/live.h"
#include "ir/simplify.h"

namespace keel
{
namespace
{
bool remove_once( Function& func, bool pin_named, const Purity& purity )
{
    const Liveness    live      = live_locals( func );
    const Local_set   addressed = addressed_locals( func );
    std::vector<bool> deleted( func.statements.size(), false );
    bool              removed = false;

    for( u32 s = 0; s < func.statements.size(); ++s )
    {
        const Statement& statement = func.statements[s];
        const Rvalue&    value     = std::as_const( func ).rvalue( statement );

        // Liveness sees locals, not pointees, so a write a pointer may read stays; so does a call or an
        // allocation, which does more than write, unless the call is of a function that only returns.
        if( statement.kind != Statement_kind::Assign || statement.place.is_global() ||
            has_deref_projection( func, statement.place ) || addressed[statement.place.local.v] ||
            ( pin_named && func.locals[statement.place.local].name.is_valid() ) ||
            ( value.kind == Rvalue_kind::Call && !purity.removable( value ) ) || value.kind == Rvalue_kind::Indirect_call ||
            value.kind == Rvalue_kind::Allocate || value.kind == Rvalue_kind::Release ||
            live.after[s][statement.place.local.v] )
        {
            continue;
        }

        deleted[s] = true;
        removed    = true;
    }

    if( removed )
    {
        remove_statements( func, deleted );
    }

    return removed;
}
} // namespace

// A write that only fed a dead one is dead next round.
// Liveness is recomputed per round, quadratic at worst; a backward walk deleting in place if it shows.
void remove_dead_stores( Function& func, bool pin_named, const Purity& purity )
{
    while( remove_once( func, pin_named, purity ) )
    {
    }
}
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <algorithm>
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

constexpr std::string_view k_types = "struct Point { i32 x; i32 y; };\n"
                                     "i32 side( i32 n ) { return n; }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it, but with addresses kept. The function under test is always the first.
struct Stored : Compiled
{
    explicit Stored( std::string_view source, std::optional<Inline_weights> inlining = Inline_weights {} )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build { .level = 2, .inlining = inlining, .forwarding = false }
          )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    std::size_t assigns( Rvalue_kind kind ) const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s ) { return s.kind == Statement_kind::Assign && first().rvalue( s ).kind == kind; }
        ) );
    }

    std::size_t field_writes() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            []( const Statement& s ) { return s.kind == Statement_kind::Assign && s.place.num_projections == 1; }
        ) );
    }
};

} // namespace

// `y`'s first value is overwritten unread, and `x` fed only that.
TEST_CASE( "remove_dead_stores_removes_a_write_only_a_dead_write_reads", "[opt][dead]" )
{
    const Stored p( "i32 f( i32 a ) { i32 x = a + 1; i32 y = x * 2; y = a; return y; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.assigns( Rvalue_kind::Binary ) == 0 );
}

// Nothing reads `q` after `q.x = 7` but `p`, which points at it.
TEST_CASE( "remove_dead_stores_keeps_a_write_a_pointer_reads", "[opt][dead]" )
{
    const Stored p( "i32 f( i32 a ) { Point q = Point { a, a }; Point* p = &q; q.x = 7; return p.x; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.field_writes() == 3 );
}

// Not inlined, so the call is still one; `side` only returns.
TEST_CASE( "remove_dead_stores_removes_an_unread_call_that_only_returns", "[opt][dead]" )
{
    const Stored p( "i32 f( i32 a ) { _ = side( a ); return a; }\n", std::nullopt );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.assigns( Rvalue_kind::Call ) == 0 );
}

// Pure, but a loop or recursion may not end, a panic ends the program, and a write is an effect.
TEST_CASE( "remove_dead_stores_keeps_an_unread_call_that_may_not_return", "[opt][dead]" )
{
    for( const std::string_view callee :
         { "i32 g( i32 n ) { i32 s = 0; while( n > s ) { s += 1; } return s; }\n",
           "i32 g( i32 n ) { return n <= 0 ? 0 : g( n - 1 ); }\n",
           "i32 g( i32 n ) { return 10 / n; }\n",
           "i32 t = 0;\ni32 g( i32 n ) { t = n; return n; }\n" } )
    {
        const Stored p( "i32 f( i32 a ) { _ = g( a ); return a; }\n" + std::string( callee ), std::nullopt );
        INFO( callee );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.first() ).empty() );
        REQUIRE( p.assigns( Rvalue_kind::Call ) == 1 );
    }
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS