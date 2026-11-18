// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// In an innermost loop, a branch whose one or two arms each hold at most `k_flattened_arm` scalar
// assignments that cannot fault, and join, becomes straight-line code: each arm computes into
// temporaries, and each local an arm assigns takes a `select` of the two. Whether it changed anything.
bool flatten_branches( Function& func, const Type_table& table );

constexpr u32 k_flattened_arm = 3;
} // namespace keel
