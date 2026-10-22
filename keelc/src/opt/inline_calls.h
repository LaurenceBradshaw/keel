// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <vector>
#include "common/literal_pool.h"
#include "ir/kir.h"

namespace keel
{
// The tuning knobs: rustc's MIR inliner's weights scaled by 5, with Go's cheap panic and big-caller cap.
struct Inline_weights
{
    u32 statement            = 1; // an assignment
    u32 call                 = 5; // a call, drop, allocation or release; a lone one is free
    u32 branch               = 1;
    u32 panic                = 1;    // cheap, so a check does not keep a body out
    u32 threshold            = 10;   // the most a callee may cost
    u32 few_blocks           = 3;    // a callee with this many blocks or fewer may cost a quarter more
    u32 big_caller           = 1000; // statements, before inlining, past which a caller takes only
    u32 big_caller_threshold = 2;    // callees costing this much
};

// Release only: a call to a small saved body becomes that body. A body not saved yet, as one in the
// caller's own group, is never taken, which is what stops recursion.
void inline_calls(
    Function&                                   caller,
    const std::vector<std::optional<Function>>& saved,
    const Literal_pool&                         literals,
    const Inline_weights&                       weights = {}
);

// Release only: an unflagged drop whose destructor is small and `finished` becomes that destructor's
// body, run on the dropped place.
void inline_drops(
    Function&                    caller,
    const std::vector<Function>& functions,
    const std::vector<bool>&     finished,
    const Literal_pool&          literals,
    const Inline_weights&        weights = {}
);

} // namespace keel
