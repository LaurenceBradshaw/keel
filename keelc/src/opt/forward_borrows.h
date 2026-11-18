// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// `(*_p)` becomes `_k` where `_p` is assigned once, `&_k` or a copy of such a pointer, and never escapes; `pin_named` keeps a
// named `_p`.
// Without `pin_named`, first `(*_q)` becomes `_a[_k]` where every write of `_q` is `&_a[_i]` or a copy, run, run's data or
// offset of such a pointer.
void forward_borrows( Function& func, const Type_table& table, bool pin_named );
} // namespace keel
