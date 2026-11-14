// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "ir/loops.h"
#include "sema/type.h"

namespace keel
{
// At -O3, after hoist_invariants: a rotated innermost loop, under the versioning budget, whose stores
// all go through `b + i` for one base `b` and `i` stepped by 1 to the latch's `i < n`, and may reach
// a read through a pointer. The preheader makes each such read the loop could make early, and tests
// that `[ b + i, b + n )` holds none a store may reach. If so the loop runs on those values; otherwise
// a copy of the loop as it was runs. Neither is versioned again. Whether it changed the function.
bool version_loops( Function& func, Type_table& types, Literal_pool& literals, const Loop_weights& weights );
} // namespace keel
