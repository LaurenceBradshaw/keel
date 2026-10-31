// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/settle_drops.h"
#include <algorithm>
#include <optional>
#include "ir/initialised.h"
#include "ir/simplify.h"

namespace keel
{
void settle_drops( Function& func )
{
    // The flags, found by the drops that read them.
    Local_set flags( func.locals.size(), false );
    for( const Statement& statement : func.statements )
    {
        if( statement.kind == Statement_kind::Drop && statement.drop_flag.is_valid() )
        {
            flags[statement.drop_flag.v] = true;
        }
    }

    if( std::ranges::find( flags, true ) == flags.end() )
    {
        return;
    }

    // Judged before any drop goes, which changes nothing occupancy sees: a drop neither fills nor empties.
    const Slot_map               slots( func );
    const std::vector<Occupancy> entry = occupancy_on_entry( func, slots );
    std::vector<bool>            deleted( func.statements.size(), false );

    for( const Block_id b : func.blocks.ids() )
    {
        const Block& block = func.blocks[b];
        Occupancy    state = entry[b.v];

        for( u32 i = 0; i < block.statement_count; ++i )
        {
            const u32  at        = func.statements_of( block ).position( i );
            Statement& statement = func.statements[at];

            // A pointee's occupancy is not tracked, so its flag stays.
            if( statement.kind != Statement_kind::Drop || !statement.drop_flag.is_valid() || statement.place.is_global() ||
                has_deref_projection( func, statement.place ) )
            {
                step( func, slots, statement, state );
                continue;
            }

            // Each drop lies inside one leaf, as drop_flags gave it its flag.
            const Slots range = slots.of( statement.place );
            assert( range.last == range.first + 1 || !range.exact );
            if( !state.initialised[range.first] )
            {
                deleted[at]         = true;
                statement.drop_flag = Local_id {};
            }
            else if( !state.uninitialised[range.first] )
            {
                statement.drop_flag = Local_id {};
            }

            step( func, slots, statement, state );
        }
    }

    remove_statements( func, deleted );
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
                                     "void consume( move Box b ) { }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Settled : Compiled
{
    explicit Settled( std::string_view source, std::optional<Inline_weights> inlining = Inline_weights {} )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build { .optimise = true, .inlining = inlining }
          )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    std::size_t drops( bool flagged ) const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s ) { return s.kind == Statement_kind::Drop && s.drop_flag.is_valid() == flagged; }
        ) );
    }

    // The one flagged drop's flag.
    Local_id flag() const
    {
        const auto drop =
            std::ranges::find_if( first().statements, []( const Statement& s ) { return s.drop_flag.is_valid(); } );
        REQUIRE( drop != first().statements.end() );
        return drop->drop_flag;
    }

    // Whole writes to `local`, in block `only` when given.
    std::size_t writes_to( Local_id local, std::optional<u32> only = {} ) const
    {
        const Function& func  = first();
        std::size_t     count = 0;
        for( const Block_id b : func.blocks.ids() )
        {
            for( const Statement& s : func.statements_of( func.blocks[b] ) )
            {
                count += ( !only || *only == b.v ) && s.kind == Statement_kind::Assign && s.place.local == local &&
                         s.place.num_projections == 0;
            }
        }
        return count;
    }
};

} // namespace

// Not inlined, so `consume`'s own drop of `b` stays out of `f`.
TEST_CASE( "settle_drops_deletes_a_drop_moved_on_every_path", "[opt][settle]" )
{
    const Settled p( "i32 f() { { Box b = Box( 1 ); consume( move b ); } return 0; }\n", std::nullopt );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( true ) == 0 );
    REQUIRE( p.drops( false ) == 0 );
}

// The old value drops before the reassignment and the new one at scope end, both certainly full.
// Not inlined, so `~Box`'s empty body does not take the drops with it.
TEST_CASE( "settle_drops_unflags_a_drop_built_on_every_path", "[opt][settle]" )
{
    const Settled p( "i32 f() { Box b = Box( 1 ); b = Box( 2 ); return b.v; }\n", std::nullopt );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( true ) == 0 );
    REQUIRE( p.drops( false ) == 2 );
}

// Moved on one path: the flag stays, set where `b` is built and cleared where it moves, nowhere else.
TEST_CASE( "settle_drops_keeps_a_flag_where_paths_differ", "[opt][settle]" )
{
    const Settled p( "i32 f( bool c ) { { Box b = Box( 3 ); if( c ) { consume( move b ); } } return 0; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( true ) == 1 );
    REQUIRE( p.writes_to( p.flag() ) == 2 );
}

// `p.a` is filled on every path and `p.b` on one: only `p.b`'s drop keeps its flag.
TEST_CASE( "settle_drops_judges_each_field_on_its_own", "[opt][settle]" )
{
    const Settled p(
        "class Pair { public Box a; public Box b; };\n"
        "i32 f( bool c ) { Pair p; p.a = Box( 1 ); if( c ) { p.b = Box( 2 ); } return 0; }\n",
        std::nullopt
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( true ) == 1 );
    REQUIRE( p.drops( false ) == 1 );
}

// The temporary is built on one arm only. Its flag's write on entry looks dead, being false like the
// clears that follow, but the arm that skips the temporary reads it.
TEST_CASE( "settle_drops_keeps_the_entry_clear_a_skipping_path_reads", "[opt][settle]" )
{
    const Settled p( "i32 f( bool c ) { return c ? Box( 5 ).v : 6; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( true ) == 1 );
    REQUIRE( p.writes_to( p.flag(), 0 ) == 1 );
    REQUIRE( p.writes_to( p.flag() ) == 2 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
