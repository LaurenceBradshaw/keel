// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// After settle_drops: a local holding one constant on every path that runs reads as that constant, an
// operation on constants becomes its value, and a branch on a constant loses the arm it never takes;
// release only. `relating`: a test of two locals an earlier one on the path decides folds too.
void propagate_constants( Function& func, const Type_table& types, Literal_pool& literals, bool relating = false );
} // namespace keel
