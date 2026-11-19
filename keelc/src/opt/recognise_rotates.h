// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// `( x << c ) | ( x >> ( w - c ) )` of an unsigned `x` of `w` bits, the shifts earlier in the `|`'s
// block, with a constant `c` and no write of `x` between, becomes a rotate of `x` by `c`. The shifts
// are left for remove_dead_stores. Whether it changed anything.
bool recognise_rotates( Function& func, const Type_table& table, const Literal_pool& literals );
} // namespace keel
