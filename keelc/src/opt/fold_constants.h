// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// An operation on constants becomes its value, wrapped at its type's width (§12); answers whether
// anything folded.
bool fold_constants( Function& func, const Type_table& types, Literal_pool& literals );
} // namespace keel