// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ir/kir.h"

namespace keel
{

// How far `-O3`'s loop passes may grow a function; each budget arrives with its pass.
struct Loop_weights
{
    u32 rotation   = 8;   // the most statements a loop's test may hold, storage markers aside, to be copied
    u32 unrolling  = 200; // the most statements a fully unrolled loop may hold, its trip count times its body (-O3)
    u32 versioning = 200; // the most statements a loop may hold to be copied behind a run-time overlap check (-O3)
};

// Each block's immediate dominator, Cooper–Harvey–Kennedy over reverse postorder.
struct Dominators
{
    std::vector<Block_id> idom;  // invalid for an unreachable block; the entry is its own
    std::vector<u32>      order; // reverse postorder position; unreachable blocks have none

    bool dominates( Block_id a, Block_id b ) const;
};

Dominators find_dominators( const Function& func );

constexpr u32 k_no_loop = ~u32 { 0 };

// A natural loop: its header and every block that reaches a back edge without passing the header.
struct Loop
{
    Block_id              header {};
    std::vector<Block_id> blocks;       // ascending, the header among them
    std::vector<Block_id> exits;        // ascending: the blocks outside that an edge from inside enters
    Block_id              preheader {}; // the one block outside entering the header, by a goto; invalid if none
    Block_id              latch {};     // the one block a back edge leaves; invalid if several
    u32                   parent = k_no_loop;

    bool contains( Block_id block ) const;
};

// Innermost first, so a loop's parent comes after it. A cycle with no back edge is not a loop.
struct Loops
{
    Dominators        dominators;
    std::vector<Loop> loops;
};

Loops find_loops( const Function& func );

// Gives each loop a preheader, one latch, and exits nothing outside it enters; whether it added a block.
bool normalise_loops( Function& func );

// Each normalised loop that tests at its header gets the test copied to its latch, so the header
// becomes a guard before the loop; then normalises again. Whether it rotated any.
bool rotate_loops( Function& func, const Loop_weights& weights );

// A copy of `loop`'s blocks, its edges inside the loop kept inside the copy; `map` takes each block to its copy.
void clone_blocks( Function& func, const Loop& loop, std::vector<Block_id>& map );

} // namespace keel
