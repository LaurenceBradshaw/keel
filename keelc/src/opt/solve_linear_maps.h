// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <vector>
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "ir/purity.h"
#include "linear/recognise.h"
#include "linear/solve.h"
#include "linear/sums.h"
#include "sema/type.h"

namespace keel
{
struct Map_outcome
{
    bool                        solved = false; // the body is now the circuit
    bool                        cached = false; // the circuit came from the cache, not a solve
    std::optional<Unrecognised> ending = {};    // why no map was found
    std::optional<Cost>         cost   = {};    // what the body costs after, when a map was read
};

// keelc's functions for the kernels plans call, made once a program: the kernels made, and how one joins the
// functions, optimised.
struct Planned_kernels
{
    std::set<Kernel>                made;
    std::function<void( Function )> add;
};

// At -O3, first in the loop phase, on a `@real_arithmetic` function: the linear map its body computes,
// recognised, solved, and lowered in place of the whole body when the circuit costs less than the body.
// A nonempty `cache` folder is looked in before solving and keeps what is solved. A part of a
// plan that would loop is a call of a function `kernels` makes.
Map_outcome solve_linear_maps(
    Function&                    func,
    const std::vector<Function>& functions,
    const Purity&                purity,
    Type_table&                  types,
    Literal_pool&                literals,
    const std::filesystem::path& cache   = {},
    const Solve_options&         options = {},
    const Global_constants&      globals = {},
    Planned_kernels*             kernels = nullptr
);

} // namespace keel
