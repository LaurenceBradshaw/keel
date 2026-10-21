// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ir/kir.h"

namespace keel
{

// What each local might hold at a point, forward over KIR with union at a join. Read by M11's
// optimisations to tell when two locals' values are never alive at once.

// Indexed by Local_id::v.
using Local_set = std::vector<bool>;

struct Occupancy
{
    Local_set initialised;  // might hold a value here
    Local_set storage_dead; // might be outside its storage_live ... storage_dead here
};

// Carries `state` across one statement, so a caller can see every point in a block.
void step( const Function& func, const Statement& statement, Occupancy& state );

// Carries `state` across a terminator; only a moving condition changes it.
void step( const Terminator& terminator, Occupancy& state );

// Whether `statement` puts a value, or part of one, in `local`: an assignment to it or a field of it, or its constructor's
// address.
bool writes( const Function& func, const Statement& statement, Local_id local );

// The state at the top of each block, indexed by Block_id::v.
std::vector<Occupancy> occupancy_on_entry( const Function& func );

} // namespace keel