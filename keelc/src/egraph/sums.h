// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <compare>
#include <filesystem>
#include <string>
#include <vector>
#include "egraph/circuit.h"
#include "egraph/solve.h"

namespace keel
{
// y[k] = sum over j < n of x[j] W_M^(c j k), for k < m, W_M = exp(-2pii/M).
struct Kernel
{
    u32 n;
    u32 m;
    u64 M;
    u64 c;

    auto operator<=>( const Kernel& ) const = default;
};

struct Planned
{
    Circuit          circuit { false };
    std::vector<u32> outputs; // the node of each k
    Cost             cost;
    std::string      how;
};

struct Plan_options
{
    u32                   leaf_max = 64; // largest side handed to solve
    Solve_options         solve;
    std::filesystem::path cache {}; // where solved leaves are kept, as solve_linear_maps keeps whole maps
};

// The cheapest circuit found from the exponent's laws alone: W^(a+b) = W^a W^b and W^M = 1 make
// equal columns and rows, and split j = j1 + P j2, k = k2 + R k1 into a sum over j2 for each j1,
// a twiddle W^(c j1 k2), and a sum over j1 for each k2, once the cross term c P R j2 k1 is 0 mod M.
// Small kernels are solved.
Planned plan_sum( const Kernel& kernel, const Plan_options& options = {} );
} // namespace keel
