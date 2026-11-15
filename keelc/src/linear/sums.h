// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <array>
#include <complex>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "linear/circuit.h"
#include "linear/solve.h"

namespace keel
{
// An integer polynomial in j and k, keyed by the power of each; no zero coefficient.
using Exponent = std::map<std::array<u8, 2>, i64>;

// y[k] = sum over j < n of x[j] times the sum over e of c W_M^e(j, k), for k < m, W_M = exp(-2pii/M).
struct Sum
{
    u32                                   n;
    u32                                   m;
    u64                                   M;
    std::map<Exponent, std::complex<f64>> coefficient;

    bool operator==( const Sum& ) const = default;
};

// y[k] = sum over j < n of x[j] W_M^(c j k).
Sum exponent_kernel( u32 n, u32 m, u64 M, u64 c );

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

// The cheapest circuit the planner's rules find, each a law of the exponent: W^M = 1 folds equal
// columns and rows, and j = j1 + P j2, k = k2 + R k1 splits the sum into sums over j2, a twiddle
// W^(c j1 k2) and sums over j1, once the cross term c P R j2 k1 is 0 mod M. Small kernels are
// solved. Nothing when no rule takes the sum.
std::optional<Planned> plan_sum( const Sum& sum, const Plan_options& options = {} );
} // namespace keel
