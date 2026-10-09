// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"

namespace keel
{

// What a flag is made of, passed as data so the pass stays a function of its inputs.
struct Flag_vocabulary
{
    Type_id    bool_type {};
    Literal_id false_literal {};
    Literal_id true_literal {};
};

// PLAN §7. A local moved, or a temporary built, on only some paths gets a hidden bool tested at its
// drop: set once assigned, cleared once moved or its storage ends. Exact by construction, since it
// records what happened, so it needs no dataflow.
void elaborate_drops( Function& func, const Flag_vocabulary& vocabulary );

} // namespace keel
