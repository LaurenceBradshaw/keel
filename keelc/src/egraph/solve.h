// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "common/types.h"
#include "egraph/circuit.h"
#include "egraph/field.h"

namespace keel
{
// Work is counted in steps, not seconds, so every machine builds the same circuit.
struct Solve_options
{
    u64 fuel         = 1'000'000'000; // the whole run, 0 unlimited
    u64 graph_fuel   = 10'000'000;    // per direct search's graph
    u64 restart_fuel = 3'000'000;     // per direct search's extraction restart
    u32 restarts     = 16;
    u32 direct_max   = 9; // largest side searched directly once another candidate has solved it
};

struct Solution
{
    Circuit circuit; // an output per nonzero row
    Cost    cost;
    Cost    input; // the input program
    u64     spent;
    u32     memo;   // sub-maps solved
    u32     reused; // sub-maps solved as another's circuit up to order and units
};

// The cheapest circuit found for the map: the input program, or a split into sub-maps (components,
// input blocks, a diagonal, rotations, a Hankel core by DFT or CRT, Toom-2/3) solved the same way,
// memoised up to row and column order and units, or a direct graph search.
Solution solve( Field& field, const std::vector<Row>& rows, const Solve_options& options = {} );
} // namespace keel
