// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// Plain Keel a machine does in one instruction becomes the Rvalue kind that says so:
// - `( x << c ) | ( x >> ( w - c ) )` of an unsigned `x` of `w` bits, the shifts earlier in the `|`'s
//   block with a constant `c` and no write of `x` between, a Rotate of `x` by `c`;
// - a loop block that, while `x != 0`, only steps `x` and adds or takes 1 from `n`, entered only
//   with `x != 0`, one add of what it counts: Count_ones for `x &= x - 1`, the bits below the
//   highest set for `x >>= 1`. `x` ends 0 and the block leaves the loop.
// Whether it changed anything.
bool recognise_idioms( Function& func, const Type_table& table, Literal_pool& literals );
} // namespace keel
