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

// y[k] = sum over j < n of x[j] times the sum over e of c W_M^e(j, k), plus b(k), for k < m, W_M = exp(-2pii/M);
// b(k) is the sum over e of c W_M^e(k). Each c is rational, as an irrational is a sum of powers of W.
// A batch is that many copies on separate inputs, x[t n + j] giving y[t m + k].
struct Sum
{
    u32                          n;
    u32                          m;
    u64                          M;
    std::map<Exponent, Rational> coefficient;
    bool                         real = false; // x is real
    std::map<Exponent, Rational> offset {};    // b, its exponents in k alone
    u32                          batch = 1;

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
    u32                   leaf_max  = 64;         // largest side handed to solve
    u64                   leaf_fuel = 10'000'000; // most of solve's fuel a leaf gets, as other routes compete with it
    Solve_options         solve;
    std::filesystem::path cache {}; // where solved leaves are kept, as solve_linear_maps keeps whole maps
    // Leaves solved with these options, by n, m, M, c and whether x is real.
    mutable std::map<std::array<u64, 5>, Circuit> solved {};
    // The cheapest function promising DFT_n, made when first asked for; nothing when none is.
    std::function<std::optional<Promised>( u64 n )> promised {};
};

// Element i of copy t of a buffer: offset + step i + batch t, plus each enclosing loop's index times its coefficient.
struct Stride
{
    u32                              buffer = 0;
    u64                              offset = 0;
    u64                              step   = 1;
    u64                              batch  = 0;
    std::vector<std::pair<u32, u64>> loops {};
};

enum class Loop_kind : u8
{
    Leaf,    // leaves[leaf] from its `count` inputs at `in` to `out`
    Loop,    // `body` `count` times, its index loop `loop`
    Twiddle, // element a R + b of `out` times tables[table][c a b mod M], for 0 < a < count, 0 < b < R
    Scale,   // out[i] = in[i] times tables[table][i], for i < count
};

struct Loop_step
{
    Loop_kind              kind;
    u32                    count = 0;
    u32                    loop  = 0;
    std::vector<Loop_step> body {};
    u32                    leaf  = 0;
    u32                    table = 0;
    u64                    R     = 0;
    u64                    M     = 0;
    u64                    c     = 0;
    Stride                 in {};
    Stride                 out {};
};

// A plan as loops over straight-line leaves. Buffer 0 is x and 1 is y; the rest are temporaries of buffers[b] elements.
struct Loop_program
{
    std::vector<Planned>                        leaves;
    std::vector<std::vector<std::complex<f64>>> tables;
    std::vector<u64>                            buffers { 0, 0 };
    std::vector<Loop_step>                      steps;
    u32                                         loops = 0;
    Cost                                        cost;
};

// The cheapest circuit the planner's rules find, each a law of the sum: an offset is added after, or to
// the one input whose column it is a multiple of, terms in j or k alone factor
// out, a real x pairs conjugate terms, W^M = 1 folds equal columns and rows, and j = j1 + P j2,
// k = k2 + R k1 splits the sum into sums over j2, a twiddle W^(c j1 k2) and sums over j1, once the
// cross term c P R j2 k1 is 0 mod M; two real copies of a hermitian kernel are one complex sum. Columns
// equal up to sign under a reflection fold or reorder, a j power that is a multiple of the jk one shifts
// the outputs, and a sum may be its transpose's circuit run backwards. A complex DFT of coprime factors is
// theirs with no twiddle (Good-Thomas), and a prime one a cyclic convolution of length p - 1 (Rader).
// Small kernels are solved. Nothing when no rule takes the sum.
std::optional<Planned> plan_sum( const Sum& sum, const Plan_options& options = {} );

// The same plan with each split, chirp and convolution wider than leaf_max a loop over its parts; a complex x only.
std::optional<Loop_program> plan_loops( const Sum& sum, const Plan_options& options = {} );
} // namespace keel
