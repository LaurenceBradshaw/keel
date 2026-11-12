// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/loops.h"
#include <algorithm>
#include <utility>

namespace keel
{
namespace
{

std::vector<std::vector<Block_id>> predecessors( const Function& func )
{
    std::vector<std::vector<Block_id>> preds( func.blocks.size() );
    std::vector<Block_id>              targets;
    for( const Block_id b : func.blocks.ids() )
    {
        targets.clear();
        successors( func.blocks[b].terminator, targets );
        for( const Block_id target : targets )
        {
            preds[target.v].push_back( b );
        }
    }
    return preds;
}

// Reachable blocks, the entry first.
std::vector<Block_id> reverse_postorder( const Function& func )
{
    std::vector<Block_id>                 order;
    std::vector<bool>                     seen( func.blocks.size(), false );
    std::vector<std::pair<Block_id, u32>> stack { { k_entry_block, 0 } };
    std::vector<Block_id>                 next;
    seen[k_entry_block.v] = true;

    while( !stack.empty() )
    {
        const auto [b, i] = stack.back();
        next.clear();
        successors( func.blocks[b].terminator, next );
        if( i == next.size() )
        {
            order.push_back( b );
            stack.pop_back();
            continue;
        }

        stack.back().second++;
        if( !seen[next[i].v] )
        {
            seen[next[i].v] = true;
            stack.push_back( { next[i], 0 } );
        }
    }

    std::reverse( order.begin(), order.end() );
    return order;
}

// Points each of `from`'s edges into `to` at a new block that jumps to `to`.
void detour( Function& func, const std::vector<Block_id>& from, Block_id to )
{
    const Block_id via = func.blocks.push_back( Block {
        .first_statement = narrow_cast<u32>( func.statements.size() ),
        .terminator = Terminator { .kind = Terminator_kind::Goto, .span = func.blocks[to].terminator.span, .targets = { to } },
    } );

    for( const Block_id b : from )
    {
        for( Block_id& target : func.blocks[b].terminator.targets )
        {
            if( target == to )
            {
                target = via;
            }
        }
    }
}

// The entry's body moves to a new block, so the entry can be a loop's preheader.
void move_entry( Function& func )
{
    const Block_id moved = func.blocks.push_back( func.blocks[k_entry_block] );
    for( Block& block : func.blocks )
    {
        for( Block_id& target : block.terminator.targets )
        {
            if( target == k_entry_block )
            {
                target = moved;
            }
        }
    }

    func.blocks[k_entry_block] = Block {
        .terminator =
            Terminator { .kind = Terminator_kind::Goto, .span = func.blocks[moved].terminator.span, .targets = { moved } },
    };
}

std::vector<Block_id> inside( const Loop& loop, const std::vector<Block_id>& blocks, bool in )
{
    std::vector<Block_id> chosen;
    for( const Block_id b : blocks )
    {
        if( loop.contains( b ) == in )
        {
            chosen.push_back( b );
        }
    }
    return chosen;
}

// Whether `loop` needed a block; the graph is then stale.
bool normalise( Function& func, const Loop& loop, const std::vector<std::vector<Block_id>>& preds )
{
    if( loop.header == k_entry_block )
    {
        move_entry( func );
        return true;
    }

    bool added = false;
    if( !loop.preheader.is_valid() )
    {
        detour( func, inside( loop, preds[loop.header.v], false ), loop.header );
        added = true;
    }
    if( !loop.latch.is_valid() )
    {
        detour( func, inside( loop, preds[loop.header.v], true ), loop.header );
        added = true;
    }
    for( const Block_id exit : loop.exits )
    {
        if( !inside( loop, preds[exit.v], false ).empty() )
        {
            detour( func, inside( loop, preds[exit.v], true ), exit );
            added = true;
        }
    }
    return added;
}

// A copy of the header's test at the latch, when the header exits and the latch only jumps back.
bool rotate( Function& func, const Loop& loop, const Loop_weights& weights )
{
    const Block      header = func.blocks[loop.header];
    const Terminator test   = header.terminator;
    if( !loop.preheader.is_valid() || !loop.latch.is_valid() ||
        func.blocks[loop.latch].terminator.kind != Terminator_kind::Goto || test.kind != Terminator_kind::Branch ||
        loop.contains( test.targets[0] ) == loop.contains( test.targets[1] ) )
    {
        return false;
    }

    u32 size = 0;
    for( const Statement& statement : func.statements_of( header ) )
    {
        size += statement.kind == Statement_kind::Assign || statement.kind == Statement_kind::Drop;
    }
    if( size > weights.rotation )
    {
        return false;
    }

    const u32 first = narrow_cast<u32>( func.statements.size() );
    for( u32 s = 0; s < header.statement_count; ++s )
    {
        Statement copy = func.clone( func.statements[header.first_statement + s] );
        func.statements.push_back( std::move( copy ) );
    }
    const Block_id bottom =
        func.blocks.push_back( Block { .first_statement = first, .statement_count = header.statement_count, .terminator = test }
        );
    func.blocks[loop.latch].terminator.targets[0] = bottom;
    return true;
}

} // namespace

bool Dominators::dominates( Block_id a, Block_id b ) const
{
    if( !idom[b.v].is_valid() )
    {
        return false;
    }
    for( ;; )
    {
        if( b == a )
        {
            return true;
        }
        if( b == k_entry_block )
        {
            return false;
        }
        b = idom[b.v];
    }
}

Dominators find_dominators( const Function& func )
{
    const std::vector<Block_id>              rpo   = reverse_postorder( func );
    const std::vector<std::vector<Block_id>> preds = predecessors( func );

    Dominators dom {
        .idom = std::vector<Block_id>( func.blocks.size() ), .order = std::vector<u32>( func.blocks.size(), k_no_loop )
    };
    for( u32 r = 0; r < rpo.size(); ++r )
    {
        dom.order[rpo[r].v] = r;
    }

    const auto intersect = [&]( Block_id a, Block_id b )
    {
        while( a != b )
        {
            while( dom.order[a.v] > dom.order[b.v] )
            {
                a = dom.idom[a.v];
            }
            while( dom.order[b.v] > dom.order[a.v] )
            {
                b = dom.idom[b.v];
            }
        }
        return a;
    };

    dom.idom[k_entry_block.v] = k_entry_block;
    for( bool changed = true; changed; )
    {
        changed = false;
        for( u32 r = 1; r < rpo.size(); ++r )
        {
            const Block_id b = rpo[r];
            Block_id       best {};
            for( const Block_id p : preds[b.v] )
            {
                if( dom.idom[p.v].is_valid() )
                {
                    best = best.is_valid() ? intersect( p, best ) : p;
                }
            }
            if( best != dom.idom[b.v] )
            {
                dom.idom[b.v] = best;
                changed       = true;
            }
        }
    }

    return dom;
}

bool Loop::contains( Block_id block ) const
{
    return std::binary_search( blocks.begin(), blocks.end(), block );
}

Loops find_loops( const Function& func )
{
    Loops                                    found { .dominators = find_dominators( func ), .loops = {} };
    const Dominators&                        dom   = found.dominators;
    const std::vector<std::vector<Block_id>> preds = predecessors( func );

    std::vector<Block_id> targets;
    for( const Block_id header : func.blocks.ids() )
    {
        std::vector<Block_id> back;
        for( const Block_id p : preds[header.v] )
        {
            if( dom.dominates( header, p ) )
            {
                back.push_back( p );
            }
        }
        if( back.empty() )
        {
            continue;
        }

        // Backward from each back edge, stopping at the header.
        std::vector<bool> in( func.blocks.size(), false );
        in[header.v]                = true;
        std::vector<Block_id> stack = back;
        while( !stack.empty() )
        {
            const Block_id b = stack.back();
            stack.pop_back();
            if( in[b.v] )
            {
                continue;
            }
            in[b.v] = true;
            for( const Block_id p : preds[b.v] )
            {
                if( dom.idom[p.v].is_valid() )
                {
                    stack.push_back( p );
                }
            }
        }

        Loop loop;
        loop.header = header;
        loop.latch  = back.size() == 1 ? back.front() : Block_id {};
        for( const Block_id b : func.blocks.ids() )
        {
            if( !in[b.v] )
            {
                continue;
            }
            loop.blocks.push_back( b );
            targets.clear();
            successors( func.blocks[b].terminator, targets );
            for( const Block_id t : targets )
            {
                if( !in[t.v] )
                {
                    loop.exits.push_back( t );
                }
            }
        }
        std::sort( loop.exits.begin(), loop.exits.end() );
        loop.exits.erase( std::unique( loop.exits.begin(), loop.exits.end() ), loop.exits.end() );

        // The entry has the call as a predecessor too.
        const std::vector<Block_id> outside = inside( loop, preds[header.v], false );
        if( header != k_entry_block && outside.size() == 1 &&
            func.blocks[outside.front()].terminator.kind == Terminator_kind::Goto )
        {
            loop.preheader = outside.front();
        }

        found.loops.push_back( std::move( loop ) );
    }

    // A loop inside another has fewer blocks.
    std::sort(
        found.loops.begin(),
        found.loops.end(),
        []( const Loop& a, const Loop& b )
        { return std::pair( a.blocks.size(), a.header ) < std::pair( b.blocks.size(), b.header ); }
    );
    for( u32 l = 0; l < found.loops.size(); ++l )
    {
        for( u32 outer = l + 1; outer < found.loops.size(); ++outer )
        {
            if( found.loops[outer].contains( found.loops[l].header ) )
            {
                found.loops[l].parent = outer;
                break;
            }
        }
    }

    return found;
}

bool normalise_loops( Function& func )
{
    bool added = false;
    for( bool again = true; again; )
    {
        again                                          = false;
        const Loops                              found = find_loops( func );
        const std::vector<std::vector<Block_id>> preds = predecessors( func );
        for( const Loop& loop : found.loops )
        {
            if( normalise( func, loop, preds ) )
            {
                again = added = true;
                break;
            }
        }
    }
    return added;
}

bool rotate_loops( Function& func, const Loop_weights& weights )
{
    bool rotated = false;
    for( bool again = true; again; )
    {
        again = false;
        normalise_loops( func );
        for( const Loop& loop : find_loops( func ).loops )
        {
            if( rotate( func, loop, weights ) )
            {
                again = rotated = true;
                break;
            }
        }
    }
    return rotated;
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{
namespace
{

Function graph( const std::vector<Terminator>& terminators )
{
    Function func;
    for( const Terminator& terminator : terminators )
    {
        func.blocks.push_back( Block { .terminator = terminator } );
    }
    return func;
}

Terminator jump( u32 to )
{
    return Terminator { .kind = Terminator_kind::Goto, .targets = { Block_id { to } } };
}

Terminator branch( u32 taken, u32 not_taken )
{
    return Terminator { .kind = Terminator_kind::Branch, .targets = { Block_id { taken }, Block_id { not_taken } } };
}

Terminator done()
{
    return Terminator { .kind = Terminator_kind::Return };
}

std::vector<Block_id> ids( std::initializer_list<u32> vs )
{
    std::vector<Block_id> out;
    for( const u32 v : vs )
    {
        out.push_back( Block_id { v } );
    }
    return out;
}

// `count` assignments, the block's statements, appended.
void fill( Function& func, u32 block, u32 count )
{
    func.blocks[Block_id { block }].first_statement = narrow_cast<u32>( func.statements.size() );
    func.blocks[Block_id { block }].statement_count = count;
    for( u32 s = 0; s < count; ++s )
    {
        func.statements.push_back( Statement { .value = func.add_rvalue( Rvalue {} ) } );
    }
}

// Every loop has a preheader and one latch, and only it enters each of its exits.
void require_normal( const Function& func )
{
    const Loops                              found = find_loops( func );
    const std::vector<std::vector<Block_id>> preds = predecessors( func );
    for( const Loop& loop : found.loops )
    {
        INFO( "loop at " << loop.header.v );
        REQUIRE( loop.preheader.is_valid() );
        REQUIRE( loop.latch.is_valid() );
        for( const Block_id exit : loop.exits )
        {
            REQUIRE( inside( loop, preds[exit.v], false ).empty() );
        }
    }
}

} // namespace

// 0 → 1 (outer header) → 2 → 3 (inner header) ⇄ 4; 3 → 5 → 1; 1 → 6.
TEST_CASE( "find_loops_nests_an_inner_loop_in_its_outer", "[ir][loops]" )
{
    Function    func  = graph( { jump( 1 ), branch( 2, 6 ), jump( 3 ), branch( 4, 5 ), jump( 3 ), jump( 1 ), done() } );
    const Loops found = find_loops( func );

    REQUIRE( found.loops.size() == 2 );
    REQUIRE( found.loops[0].header == Block_id { 3 } );
    REQUIRE( found.loops[0].blocks == ids( { 3, 4 } ) );
    REQUIRE( found.loops[0].parent == 1 );
    REQUIRE( found.loops[1].blocks == ids( { 1, 2, 3, 4, 5 } ) );
    REQUIRE( found.loops[1].parent == k_no_loop );
    REQUIRE( found.loops[1].preheader == Block_id { 0 } );
    REQUIRE( found.dominators.dominates( Block_id { 1 }, Block_id { 5 } ) );
    REQUIRE( !found.dominators.dominates( Block_id { 4 }, Block_id { 5 } ) );
    REQUIRE( !normalise_loops( func ) );
}

// Blocks 2 and 3 both jump back to 1: one loop, given one latch.
TEST_CASE( "normalise_loops_joins_two_back_edges_in_one_latch", "[ir][loops]" )
{
    Function func = graph( { jump( 1 ), branch( 2, 4 ), branch( 1, 3 ), jump( 1 ), done() } );
    REQUIRE( find_loops( func ).loops.size() == 1 );
    REQUIRE( !find_loops( func ).loops[0].latch.is_valid() );

    REQUIRE( normalise_loops( func ) );
    require_normal( func );
    REQUIRE( find_loops( func ).loops.size() == 1 );
}

// The entry's branch skips the loop to the block a `break` leaves by, and enters the header.
TEST_CASE( "normalise_loops_splits_a_break_exit_the_outer_code_enters", "[ir][loops]" )
{
    Function func = graph( { branch( 1, 3 ), branch( 2, 3 ), jump( 1 ), done() } );
    REQUIRE( find_loops( func ).loops[0].exits == ids( { 3 } ) );

    REQUIRE( normalise_loops( func ) );
    require_normal( func );
    const Loop loop = find_loops( func ).loops[0];
    REQUIRE( loop.exits.size() == 1 );
    REQUIRE( loop.exits[0] != Block_id { 3 } );
    REQUIRE( func.blocks[loop.exits[0]].terminator.targets[0] == Block_id { 3 } );
}

// A loop at the entry: the entry moves, and the empty entry is the preheader.
TEST_CASE( "normalise_loops_moves_an_entry_that_heads_a_loop", "[ir][loops]" )
{
    Function func = graph( { branch( 0, 1 ), done() } );
    REQUIRE( normalise_loops( func ) );
    require_normal( func );
    REQUIRE( find_loops( func ).loops[0].preheader == k_entry_block );
}

// 1 and 2 each enter the other, and neither dominates: no back edge, so no loop.
TEST_CASE( "find_loops_leaves_an_irreducible_region", "[ir][loops]" )
{
    Function func = graph( { branch( 1, 2 ), branch( 2, 3 ), branch( 1, 3 ), done() } );
    REQUIRE( find_loops( func ).loops.empty() );
    REQUIRE( !normalise_loops( func ) );
}

// 0 → 1 (header: test) → 2 (body) → 3 (latch) → 1; 1 → 4. The test moves to the bottom.
TEST_CASE( "rotate_loops_copies_the_test_to_the_latch", "[ir][loops]" )
{
    Function func = graph( { jump( 1 ), branch( 2, 4 ), jump( 3 ), jump( 1 ), done() } );
    fill( func, 1, 2 );
    REQUIRE( rotate_loops( func, Loop_weights {} ) );
    require_normal( func );

    const Loops found = find_loops( func );
    REQUIRE( found.loops.size() == 1 );
    const Loop& loop = found.loops[0];
    REQUIRE( loop.header == Block_id { 2 } );
    REQUIRE( !loop.contains( Block_id { 1 } ) );
    const Block& bottom = func.blocks[loop.latch];
    REQUIRE( bottom.terminator.kind == Terminator_kind::Branch );
    REQUIRE( bottom.statement_count == 2 );
    REQUIRE( bottom.first_statement != func.blocks[Block_id { 1 }].first_statement );
    REQUIRE( func.blocks[Block_id { 1 }].terminator.kind == Terminator_kind::Branch );
    REQUIRE( !rotate_loops( func, Loop_weights {} ) );
}

TEST_CASE( "rotate_loops_leaves_a_test_over_the_budget", "[ir][loops]" )
{
    Function func = graph( { jump( 1 ), branch( 2, 4 ), jump( 3 ), jump( 1 ), done() } );
    fill( func, 1, 3 );
    REQUIRE( !rotate_loops( func, Loop_weights { .rotation = 2 } ) );
    REQUIRE( find_loops( func ).loops[0].header == Block_id { 1 } );
}

// `while( true )` with a `break` at the bottom: the header has no test, and the latch already tests.
TEST_CASE( "rotate_loops_leaves_a_loop_with_no_test", "[ir][loops]" )
{
    Function func = graph( { jump( 1 ), jump( 2 ), branch( 1, 3 ), done() } );
    REQUIRE( !rotate_loops( func, Loop_weights {} ) );
    REQUIRE( func.blocks.size() == 4 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
