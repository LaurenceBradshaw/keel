// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// After propagate_constants: within a run of blocks each entered only from the last, a read of a whole
// global takes the constant or local last written to it, a write nothing reads before the next goes,
// `(x + c) - c` is `x`, and a check an earlier one proves is false; release only. Whether it changed
// anything.
bool forward_globals( Function& func, const Type_table& types, Literal_pool& literals );
} // namespace keel
