// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <vector>
#include "ir/kir.h"
#include "ir/purity.h"
#include "linear/recognise.h"
#include "linear/sums.h"
#include "sema/type.h"

namespace keel
{
struct Read_kernel
{
    Sum                     sum;
    std::vector<Value_path> inputs {};  // x[j], real and imaginary
    std::vector<Value_path> outputs {}; // y[k]
    Cost                    input {};   // the adds and muls the loops ran
};

// The kernel `func`'s loops compute, read without running them: each counted loop runs once with its
// index a symbol, an accumulator's step is summed over it, an index is a polynomial in the symbols,
// and a `@trig_*` call at a rational multiple of pi times one becomes powers of W. Nothing when the
// body is not one complex sum y[k] = sum over j of x[j] times powers of W in j and k.
std::optional<Read_kernel> read_kernel(
    const Function&              func,
    const std::vector<Function>& functions,
    const Purity&                purity,
    const Type_table&            types,
    const Literal_pool&          literals
);
} // namespace keel
