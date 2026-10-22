// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// After propagate_constants: an edge into a block that only branches on a local goes straight to the
// arm that local's value on the edge takes; release only.
void thread_jumps( Function& func, const Type_table& types, const Literal_pool& literals );
} // namespace keel
