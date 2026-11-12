// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"

namespace keel
{
// A call of `func` itself whose result it returns becomes its arguments assigned to the parameters
// and a jump back to the entry, so deep recursion needs no stack. Runs after drops are explicit.
void loop_self_calls( Function& func );
} // namespace keel
