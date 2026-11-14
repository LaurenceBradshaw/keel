// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/unroll_loops.h"
#include <algorithm>
#include <vector>
#include "ir/simplify.h"
#include "opt/forward_reads.h"
#include "opt/reduce_inductions.h"

namespace keel
{
namespace
{

// A partial unroll's copies hold at most this many statements, so a loop inlined again is not copied again.
constexpr u64 k_partial = 16;

// Fully, or at -O3 by 2 or 4 when the count is over budget and divisible: one copy keeps the test.
bool unroll(
    Function&           func,
    const Type_table&   types,
    const Literal_pool& literals,
    const Loop&         loop,
    u32                 budget,
    bool                growing,
    bool                partial
)
{
    if( !loop.preheader.is_valid() || !loop.latch.is_valid() )
    {
        return false;
    }
    const Local_set addressed = addressed_locals_of( func );
    // An element of a local array, which split_aggregates makes a local once every index is constant.
    const auto element = [&]( const Place& place )
    {
        return !place.is_global() && place.local.is_valid() && place.num_projections == 1 &&
               func.projections[place.first_projection].kind == Projection_kind::Index && !addressed[place.local.v] &&
               types.is_sized_array( func.locals[place.local].type );
    };

    u64 size  = 0;
    u64 moves = 0; // statements that copy into or out of such an element: copies of locals once unrolled
    for( const Block_id b : loop.blocks )
    {
        for( const Statement& statement : func.statements_of( b ) )
        {
            size += statement.kind == Statement_kind::Assign || statement.kind == Statement_kind::Drop;
            if( statement.kind == Statement_kind::Assign && func.rvalue( statement ).kind == Rvalue_kind::Use )
            {
                const Operand& from = func.rvalue( statement ).a;
                moves += element( statement.place ) || ( from.has_place() && element( from.place ) );
            }
        }
    }

    const std::optional<Trip> trip = trip_count( func, types, literals, loop, addressed );
    if( !trip )
    {
        return false;
    }
    const bool full  = trip->turns <= budget && size * trip->turns <= budget;
    u64        count = full ? trip->turns : 0;
    for( const u64 f : { u64 { 4 }, u64 { 2 } } )
    {
        if( !full && partial && count == 0 && trip->turns % f == 0 && size * f <= std::min<u64>( k_partial, budget ) )
        {
            count = f;
        }
    }
    if( count == 0 )
    {
        return false;
    }
    // Each copy's step and test fold.
    const u64 kept = size - moves;
    if( !growing && ( kept - std::min<u64>( kept, 2 ) ) * trip->turns > size )
    {
        return false;
    }

    // Each turn's test passes but the last's, and its back edge enters the next copy. Partially, the copy
    // where the count runs out keeps it: the last when the latch tests, else the first; the last's
    // back edge enters the first.
    const Terminator test   = func.blocks[trip->test].terminator;
    const u64        keeper = full ? count : trip->test == loop.latch ? count - 1 : 0;
    const auto       jump   = [&]( Block_id to )
    { return Terminator { .kind = Terminator_kind::Goto, .span = test.span, .targets = { to } }; };
    std::vector<std::vector<Block_id>> copies( count );
    copies[0].resize( func.blocks.size() );
    for( const Block_id b : loop.blocks )
    {
        copies[0][b.v] = b;
    }
    for( u64 k = 1; k < count; ++k )
    {
        clone_blocks( func, loop, copies[k] );
    }
    for( u64 k = 0; k < count; ++k )
    {
        const std::vector<Block_id>& map  = copies[k];
        const bool                   exit = full && k + 1 == count;
        if( k != keeper )
        {
            func.blocks[map[trip->test.v]].terminator = jump( exit ? test.targets[1] : map[test.targets[0].v] );
        }
        for( Block_id& target : func.blocks[map[loop.latch.v]].terminator.targets )
        {
            if( target == map[loop.header.v] )
            {
                target = exit ? test.targets[1] : copies[( k + 1 ) % count][loop.header.v];
            }
        }
    }
    return true;
}

} // namespace

bool unroll_loops(
    Function& func, const Type_table& types, const Literal_pool& literals, const Loop_weights& weights, bool growing
)
{
    bool unrolled = false;
    for( bool again = true; again; )
    {
        again = false;
        for( const Loop& loop : find_loops( func ).loops )
        {
            if( unroll( func, types, literals, loop, weights.unrolling, growing, false ) )
            {
                // The last copy's latch, when the test is the header's.
                remove_unreachable_blocks( func );
                again = unrolled = true;
                break;
            }
        }
    }
    if( !growing )
    {
        return unrolled;
    }
    // Then each innermost loop left, once: a partial unroll leaves a loop.
    std::vector<Block_id> innermost;
    {
        const Loops forest = find_loops( func );
        for( u32 l = 0; l < forest.loops.size(); ++l )
        {
            if( std::ranges::none_of( forest.loops, [&]( const Loop& other ) { return other.parent == l; } ) )
            {
                innermost.push_back( forest.loops[l].header );
            }
        }
    }
    for( const Block_id header : innermost )
    {
        for( const Loop& loop : find_loops( func ).loops )
        {
            if( loop.header == header && unroll( func, types, literals, loop, weights.unrolling, growing, true ) )
            {
                unrolled = true;
                break;
            }
        }
    }
    return unrolled;
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

// Loops left in the first function, optimised and elaborated at -O3.
std::size_t loops_left( std::string_view source, u32 unrolling = Loop_weights {}.unrolling, u32 level = 3 )
{
    const Compiled c(
        std::string( source ) + "i32 main() { return 0; }\n",
        Stage::Elaborated,
        Build { .level = level, .loops = Loop_weights { .unrolling = unrolling } }
    );
    INFO( c.rendered() );
    REQUIRE( c.clean() );
    REQUIRE( verify( c.functions.front() ).empty() );
    return find_loops( c.functions.front() ).loops.size();
}

// The statements in the first function's loops, at -O3.
std::size_t loop_statements( std::string_view source, u32 unrolling )
{
    const Compiled c(
        std::string( source ) + "i32 main() { return 0; }\n",
        Stage::Elaborated,
        Build { .level = 3, .loops = Loop_weights { .unrolling = unrolling } }
    );
    INFO( c.rendered() );
    REQUIRE( c.clean() );
    const Function& func = c.functions.front();
    REQUIRE( verify( func ).empty() );
    std::size_t n = 0;
    for( const Loop& loop : find_loops( func ).loops )
    {
        for( const Block_id b : loop.blocks )
        {
            n += func.statements_of( b ).size();
        }
    }
    return n;
}

} // namespace

TEST_CASE( "unroll_loops_copies_a_loop_with_a_constant_count", "[opt][unroll_loops]" )
{
    constexpr std::string_view source =
        "i64 f( const ref i64[4] a ) { i64 t = 0; for( u64 i = 0; i < 4; i += 1 ) { t += a[ i ]; } return t; }\n";
    REQUIRE( loops_left( source ) == 0 );
    REQUIRE( loops_left( source, 0 ) == 1 );
}

// At -O2 a loop only moving elements of a local array is copied: split_aggregates makes each a local.
TEST_CASE( "unroll_loops_copies_a_local_array_fill_at_O2", "[opt][unroll_loops]" )
{
    REQUIRE( loops_left( "f64 f( f64 x ) { f64[8] m = [ 0.0, .. ]; m[ 1 ] = x; return m[ 0 ] + m[ 1 ]; }\n", 200, 2 ) == 0 );
    REQUIRE(
        loops_left(
            "i64 f( const ref i64[4] a ) { i64 t = 0; for( u64 i = 0; i < 4; i += 1 ) { t += a[ i ]; } return t; }\n", 200, 2
        ) == 1
    );
}

TEST_CASE( "unroll_loops_keeps_a_loop_over_its_budget", "[opt][unroll_loops]" )
{
    REQUIRE(
        loops_left(
            "i64 f( const ref i64[200] a ) { i64 t = 0; for( u64 i = 0; i < 200; i += 1 ) { t += a[ i ]; } return t; }\n"
        ) == 1
    );
}

// Over budget but divisible: two or four turns a test, still one loop.
TEST_CASE( "unroll_loops_copies_a_long_loop_in_part", "[opt][unroll_loops]" )
{
    constexpr std::string_view source =
        "i64 f( const ref i64[64] a ) { i64 t = 0; for( u64 i = 0; i < 64; i += 1 ) { t += a[ i ]; } return t; }\n";
    REQUIRE( loops_left( source ) == 1 );
    REQUIRE( loop_statements( source, 200 ) > loop_statements( source, 1 ) );
}

TEST_CASE( "unroll_loops_keeps_a_loop_with_a_run_time_count", "[opt][unroll_loops]" )
{
    REQUIRE( loops_left( "i64 f( u64 n ) { i64 t = 0; for( u64 i = 0; i < n; i += 1 ) { t += 3; } return t; }\n" ) == 1 );
}

// The trip count bounds the turns; each copy's `break` leaves for the same block.
TEST_CASE( "unroll_loops_copies_a_loop_a_break_leaves", "[opt][unroll_loops]" )
{
    REQUIRE(
        loops_left( "i64 f( const ref i64[4] a, i64 x ) { i64 t = 0; for( u64 i = 0; i < 4; i += 1 ) { if( a[ i ] == x ) { "
                    "break; } t += a[ i ]; } return t; }\n" ) == 0
    );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
