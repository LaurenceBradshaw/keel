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

// That sum's sides, M and c: the shape the index rules take.
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
    u32                   leaf_max  = 64;         // largest side handed to solve
    u64                   leaf_fuel = 10'000'000; // most of solve's fuel a leaf gets, as other routes compete with it
    Solve_options         solve;
    std::filesystem::path cache {}; // where solved leaves are kept, as solve_linear_maps keeps whole maps
    // Leaves solved with these options, by n, m, M, c and whether x is real.
    mutable std::map<std::array<u64, 5>, Circuit> solved {};
    // A part that would loop, the root aside, is a call of a function computing it alone.
    bool calls = false;
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
    Leaf,     // leaves[leaf] from its `count` inputs at `in` to `out`
    Loop,     // `body` `count` times, its index loop `loop`
    Twiddle,  // element a R + b of `out` times tables[table][c a b mod M], for 0 < a < count, 0 < b < R
    Scale,    // out[i] = in[i] times tables[table][i], for i < count
    Gather,   // out[i] = in[indices[table][i]], for i < count; conjugated when `conj`
    Add,      // out[i] = in[i] + also[i], for i < count
    Copy,     // out[i] = in[i], for i < count
    Call,     // calls[leaf]'s program from `count` inputs at `in` to `out`, both of step 1; conjugated when `conj`
    Pack,     // out[i] = in[i] + i also[i], both real, for i < count
    Untangle, // Z at `in` into the two real copies' sums at `out` and `also`, for k < count, as `paired` does
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
    bool                   conj  = false;
    bool                   real  = false; // only in's real part is read
    Stride                 in {};
    Stride                 out {};
    Stride                 also {};
};

struct Loop_call;

// A plan as loops over straight-line leaves. Buffer 0 is x, real for a real sum, and 1 is y; the rest are
// temporaries of buffers[b] elements. A real sum reads only the real parts of its input.
struct Loop_program
{
    std::vector<Planned>                        leaves;
    std::vector<std::vector<std::complex<f64>>> tables;
    std::vector<std::vector<u64>>               indices;
    std::vector<Loop_call>                      calls;
    std::vector<u64>                            buffers { 0, 0 };
    std::vector<Loop_step>                      steps;
    u32                                         loops = 0;
    Cost                                        cost;
};

// A kernel a program calls, and its own program.
struct Loop_call
{
    Kernel       kernel;
    Loop_program program;
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

// The same plan with each split, chirp, convolution, pfa, rader, hermitian and paired wider than leaf_max a loop over its
// parts.
std::optional<Loop_program> plan_loops( const Sum& sum, const Plan_options& options = {} );
} // namespace keel
