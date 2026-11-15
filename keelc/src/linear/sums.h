// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <array>
#include <filesystem>
#include <functional>
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

// An exact rational in lowest terms, its denominator positive; Field::Overflow past i64.
struct Rational
{
    i64 num = 0;
    i64 den = 1;

    Rational() = default;
    Rational( i64 n, i64 d = 1 );

    auto     operator<=>( const Rational& ) const = default;
    Rational operator+( Rational o ) const;
    Rational operator*( Rational o ) const;

    Rational operator-() const
    {
        return { -num, den };
    }
};

// y[k] = sum over j < n of x[j] times the sum over e of c W_M^e(j, k), for k < m, W_M = exp(-2pii/M).
// Each c is rational, as an irrational is a sum of powers of W.
struct Sum
{
    u32                          n;
    u32                          m;
    u64                          M;
    std::map<Exponent, Rational> coefficient;
    bool                         real = false; // x is real

    auto operator<=>( const Sum& ) const = default;
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

// A function promising DFT_n that a plan may call, and what its planned body costs.
struct Promised
{
    u32  callee;
    Cost cost;
};

struct Plan_options
{
    u32                   leaf_max = 64; // largest side handed to solve
    Solve_options         solve;
    std::filesystem::path cache {}; // where solved leaves are kept, as solve_linear_maps keeps whole maps
    // Leaves solved with these options, by n, m, M, c and whether x is real.
    mutable std::map<std::array<u64, 5>, Circuit> solved {};
    // The cheapest function promising DFT_n, made when first asked for; nothing when none is.
    std::function<std::optional<Promised>( u64 n )> promised {};
};

// The cheapest circuit the planner's rules find, each a law of the sum: terms in j or k alone factor
// out, a real x pairs conjugate terms, W^M = 1 folds equal columns and rows, and j = j1 + P j2,
// k = k2 + R k1 splits the sum into sums over j2, a twiddle W^(c j1 k2) and sums over j1, once the
// cross term c P R j2 k1 is 0 mod M. Small kernels are solved. Nothing when no rule takes the sum.
std::optional<Planned> plan_sum( const Sum& sum, const Plan_options& options = {} );
} // namespace keel
