// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <vector>
#include "common/types.h"
#include "egraph/circuit.h"
#include "egraph/sum_graph.h"

namespace keel
{
struct Extract_options
{
    u32 restarts = 16;
    u32 seed     = 1;
    u64 fuel     = 0; // edges relaxed per restart, 0 unlimited
};

struct Extraction
{
    Circuit circuit;
    Cost    input; // the input program, in the graph's costs
    Cost    best;
    u64     spent;
};

// The cheapest circuit found in `graph` for `rows`, which built it: restarts from the input program,
// tree extraction and a forward greedy, each improved by coordinate descent. Restart 0 starts from
// the input, so the result never costs more. Empty when no choice of nodes is a circuit.
std::optional<Extraction> extract( const Sum_graph& graph, const std::vector<Row>& rows, const Extract_options& options );
} // namespace keel
