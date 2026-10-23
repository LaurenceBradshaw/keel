// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// After propagate_constants: a read of a local holding a constant or a copy of a local reads that
// instead, leaving the write to remove_dead_stores, and an identity it leaves folds; release only.
void propagate_values( Function& func, const Type_table& types, Literal_pool& literals );
} // namespace keel
