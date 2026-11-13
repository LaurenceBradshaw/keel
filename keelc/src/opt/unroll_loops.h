// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "ir/loops.h"
#include "sema/type.h"

namespace keel
{
// Last in -O3's loop phase: a normalised loop with a constant trip count (`trip_count`), whose
// statements times that count fit `weights.unrolling`, becomes that many copies of its blocks in a row;
// an exit from the body leaves each copy for the same block. The round after folds each copy's index.
// Innermost first. Whether it unrolled any.
bool unroll_loops( Function& func, const Type_table& types, const Literal_pool& literals, const Loop_weights& weights );
} // namespace keel
