// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// `_2 = &(*_1)` becomes `_2 = copy _1` where both point at one type, so coalescing can merge them;
// release only.
void collapse_borrows( Function& func, const Type_table& table );
} // namespace keel
