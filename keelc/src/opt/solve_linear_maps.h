// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <vector>
#include "common/literal_pool.h"
#include "egraph/recognise.h"
#include "egraph/solve.h"
#include "ir/kir.h"
#include "ir/purity.h"
#include "sema/type.h"

namespace keel
{
struct Map_outcome
{
    bool                        solved = false; // the body is now the circuit
    std::optional<Unrecognised> ending;         // why no map was found
};

// At -O3, first in the loop phase, on a `@real_arithmetic` function: the linear map its body computes,
// recognised, solved, and lowered in place of the whole body when the circuit costs less than the body.
Map_outcome solve_linear_maps(
    Function&                    func,
    const std::vector<Function>& functions,
    const Purity&                purity,
    const Type_table&            types,
    Literal_pool&                literals,
    const Solve_options&         options = {}
);
} // namespace keel
