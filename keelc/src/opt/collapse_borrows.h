// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// `_2 = &(*_1)` becomes `_2 = copy _1` where both point at one type, so coalescing can merge them;
// and a read through a pointer cast of a builtin local's address reads the local, as `Bits` where
// one is a float and the other an integer. From -O1.
void collapse_borrows( Function& func, const Type_table& table );
} // namespace keel
