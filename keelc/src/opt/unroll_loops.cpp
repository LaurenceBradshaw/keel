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

// A copy of `loop`'s blocks, its edges inside the loop kept inside the copy; `map` takes each block to its copy.
void clone_blocks( Function& func, const Loop& loop, std::vector<Block_id>& map )
{
    map.assign( func.blocks.size(), Block_id {} );
    for( const Block_id b : loop.blocks )
    {
        const u32 first = narrow_cast<u32>( func.statements.size() );
        const u32 count = func.blocks[b].statement_count;
        for( u32 s = 0; s < count; ++s )
        {
            Statement copy = func.clone( func.statements[func.blocks[b].first_statement + s] );
            func.statements.push_back( std::move( copy ) );
        }
        const Terminator terminator = func.blocks[b].terminator;
        map[b.v] =
            func.blocks.push_back( Block { .first_statement = first, .statement_count = count, .terminator = terminator } );
    }
    for( const Block_id b : loop.blocks )
    {
        for( Block_id& target : func.blocks[map[b.v]].terminator.targets )
        {
            if( target.is_valid() && loop.contains( target ) )
            {
                target = map[target.v];
            }
        }
    }
}

bool unroll( Function& func, const Type_table& types, const Literal_pool& literals, const Loop& loop, u32 budget, bool growing )
{
    if( !loop.preheader.is_valid() || !loop.latch.is_valid() )
    {
        return false;
    }
    u64 size = 0;
    for( const Block_id b : loop.blocks )
    {
        for( const Statement& statement : func.statements_of( b ) )
        {
            size += statement.kind == Statement_kind::Assign || statement.kind == Statement_kind::Drop;
        }
    }

    const std::optional<Trip> trip = trip_count( func, types, literals, loop, addressed_locals_of( func ) );
    if( !trip || trip->turns > budget || size * trip->turns > budget )
    {
        return false;
    }
    // Each copy's step and test fold.
    if( !growing && ( size - std::min<u64>( size, 2 ) ) * trip->turns > size )
    {
        return false;
    }

    // Each turn's test passes but the last's, and its back edge enters the next copy.
    const Terminator test = func.blocks[trip->test].terminator;
    const auto       jump = [&]( Block_id to )
    { return Terminator { .kind = Terminator_kind::Goto, .span = test.span, .targets = { to } }; };
    std::vector<std::vector<Block_id>> copies( trip->turns );
    copies[0].resize( func.blocks.size() );
    for( const Block_id b : loop.blocks )
    {
        copies[0][b.v] = b;
    }
    for( u64 k = 1; k < trip->turns; ++k )
    {
        clone_blocks( func, loop, copies[k] );
    }
    for( u64 k = 0; k < trip->turns; ++k )
    {
        const std::vector<Block_id>& map          = copies[k];
        const bool                   last         = k + 1 == trip->turns;
        func.blocks[map[trip->test.v]].terminator = jump( last ? test.targets[1] : map[test.targets[0].v] );
        for( Block_id& target : func.blocks[map[loop.latch.v]].terminator.targets )
        {
            if( target == map[loop.header.v] )
            {
                target = last ? test.targets[1] : copies[k + 1][loop.header.v];
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
            if( unroll( func, types, literals, loop, weights.unrolling, growing ) )
            {
                // The last copy's latch, when the test is the header's.
                remove_unreachable_blocks( func );
                again = unrolled = true;
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
std::size_t loops_left( std::string_view source, u32 unrolling = Loop_weights {}.unrolling )
{
    const Compiled c(
        std::string( source ) + "i32 main() { return 0; }\n",
        Stage::Elaborated,
        Build { .level = 3, .loops = Loop_weights { .unrolling = unrolling } }
    );
    INFO( c.rendered() );
    REQUIRE( c.clean() );
    REQUIRE( verify( c.functions.front() ).empty() );
    return find_loops( c.functions.front() ).loops.size();
}

} // namespace

TEST_CASE( "unroll_loops_copies_a_loop_with_a_constant_count", "[opt][unroll_loops]" )
{
    constexpr std::string_view source =
        "i64 f( const ref i64[4] a ) { i64 t = 0; for( u64 i = 0; i < 4; i += 1 ) { t += a[ i ]; } return t; }\n";
    REQUIRE( loops_left( source ) == 0 );
    REQUIRE( loops_left( source, 0 ) == 1 );
}

TEST_CASE( "unroll_loops_keeps_a_loop_over_its_budget", "[opt][unroll_loops]" )
{
    REQUIRE(
        loops_left(
            "i64 f( const ref i64[200] a ) { i64 t = 0; for( u64 i = 0; i < 200; i += 1 ) { t += a[ i ]; } return t; }\n"
        ) == 1
    );
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
