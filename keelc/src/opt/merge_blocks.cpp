// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/merge_blocks.h"
#include <vector>
#include "ir/simplify.h"

namespace keel
{
namespace
{
std::vector<Block_id> next_blocks( const Function& func, const std::vector<u32>& counts )
{
    std::vector<Block_id> next( func.blocks.size(), Block_id {} );
    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        const Block& block = func.blocks[b];
        if( block.terminator.kind == Terminator_kind::Goto )
        {
            const Block_id target = block.terminator.targets[0];
            if( target.is_valid() && counts[target.v] == 1 )
            {
                next[b] = target;
            }
        }
    }
    return next;
}
} // namespace

void merge_blocks( Function& func )
{
    const std::vector<u32>      counts = predecessor_counts( func );
    const std::vector<Block_id> next   = next_blocks( func, counts );

    // Known before any walk, so a chain's head is never taken for a block another chain absorbs.
    std::vector<bool> absorbed( func.blocks.size(), false );
    bool              any = false;
    for( const Block_id target : next )
    {
        if( target.is_valid() )
        {
            absorbed[target.v] = true;
            any                = true;
        }
    }

    if( !any )
    {
        return;
    }

    std::vector<Statement> statements;
    statements.reserve( func.statements.size() );

    for( u32 h = 0; h < func.blocks.size(); ++h )
    {
        if( absorbed[h] )
        {
            continue;
        }

        // The head is written only after the walk, which reads its old range.
        const u32  first = narrow_cast<u32>( statements.size() );
        Terminator last {};

        for( Block_id current = Block_id { h }; current.is_valid(); current = next[current.v] )
        {
            const Block& block = func.blocks[current.v];
            statements.insert(
                statements.end(),
                func.statements.begin() + block.first_statement,
                func.statements.begin() + block.first_statement + block.statement_count
            );
            last = block.terminator;
        }

        Block& head          = func.blocks[h];
        head.first_statement = first;
        head.statement_count = narrow_cast<u32>( statements.size() ) - first;
        head.terminator      = last;
    }

    func.statements = std::move( statements );
    remove_unreachable_blocks( func );
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

// Block i holds one statement, which assigns local i, so a merged run shows where each came from.
Function graph( const std::vector<Terminator>& terminators )
{
    Function func;

    for( u32 i = 0; i < terminators.size(); ++i )
    {
        func.locals.push_back( Local {} );
        func.statements.push_back( Statement { .place = Place { .local = Local_id { i } } } );
        func.blocks.push_back( Block { .first_statement = i, .statement_count = 1, .terminator = terminators[i] } );
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

std::vector<u32> run( const Function& func, u32 block )
{
    std::vector<u32> locals;
    const Block&     b = func.blocks[block];

    for( u32 i = 0; i < b.statement_count; ++i )
    {
        locals.push_back( func.statements[b.first_statement + i].place.local.v );
    }

    return locals;
}

} // namespace

// Block 2 comes before the block it jumps to, and after the one that jumps to it.
TEST_CASE( "merge_blocks_joins_a_chain", "[opt][merge]" )
{
    Function func = graph( { jump( 2 ), done(), jump( 1 ) } );
    merge_blocks( func );

    REQUIRE( func.blocks.size() == 1 );
    REQUIRE( run( func, 0 ) == std::vector<u32> { 0, 2, 1 } );
    REQUIRE( func.blocks[0].terminator.kind == Terminator_kind::Return );
    REQUIRE( func.statements.size() == 3 );
}

// Each arm's `goto` reaches a block the other arm reaches too.
TEST_CASE( "merge_blocks_keeps_a_join", "[opt][merge]" )
{
    Function func = graph( { branch( 1, 2 ), jump( 3 ), jump( 3 ), done() } );
    merge_blocks( func );

    REQUIRE( func.blocks.size() == 4 );
    REQUIRE( run( func, 3 ) == std::vector<u32> { 3 } );
}

// The header has the entry's `goto` and the back edge; the body and the exit are branch arms.
TEST_CASE( "merge_blocks_keeps_a_loop_header", "[opt][merge]" )
{
    Function func = graph( { jump( 1 ), branch( 2, 3 ), jump( 1 ), done() } );
    merge_blocks( func );

    REQUIRE( func.blocks.size() == 4 );
    REQUIRE( func.blocks[0].terminator.targets[0] == Block_id { 1 } );
}

// Block 1's only predecessor is the entry, and the entry's only jump comes from block 1: without
// the entry's extra count, each would absorb the other and no block would be left.
TEST_CASE( "merge_blocks_keeps_the_entry_a_loop_returns_to", "[opt][merge]" )
{
    Function func = graph( { jump( 1 ), jump( 0 ) } );
    merge_blocks( func );

    REQUIRE( func.blocks.size() == 1 );
    REQUIRE( run( func, 0 ) == std::vector<u32> { 0, 1 } );
    REQUIRE( func.blocks[0].terminator.targets[0] == Block_id { 0 } );
}

// A release `assert` checks nothing, and its jump goes with it.
TEST_CASE( "merge_blocks_joins_a_release_assert", "[opt][merge]" )
{
    const Compiled p(
        "i32 f( i32 a ) { assert( a > 0 ); return a + 1; }\ni32 main() { return 0; }\n",
        Stage::Elaborated,
        Build { .optimise = true }
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.functions.front().blocks.size() == 1 );
    REQUIRE( verify( p.functions.front() ).empty() );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
