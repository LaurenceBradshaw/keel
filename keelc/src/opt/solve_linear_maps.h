// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "ir/purity.h"
#include "linear/recognise.h"
#include "linear/solve.h"
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

// At -O3, first in the loop phase, on a `@real_arithmetic` function: the linear map its body computes,
// recognised, solved, and lowered in place of the whole body when the circuit costs less than the body.
// A nonempty `cache` folder is looked in before solving and keeps what is solved. A plan may call
// what `promised` gives, a function of `functions`.
Map_outcome solve_linear_maps(
    Function&                                              func,
    const std::vector<Function>&                           functions,
    const Purity&                                          purity,
    Type_table&                                            types,
    Literal_pool&                                          literals,
    const std::filesystem::path&                           cache    = {},
    const Solve_options&                                   options  = {},
    const Global_constants&                                globals  = {},
    const std::function<std::optional<Promised>( u64 n )>& promised = {}
);

} // namespace keel
