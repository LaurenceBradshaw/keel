// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// In -O3's loop phase, on normalised loops, for `i` a local whose one write in the loop is `i = i + s`,
// `s` invariant: each `i * c`, `c` invariant and not a power of two, becomes a copy of a new local set
// in the preheader and stepped beside `i`. The other forms (`i + c`, `i - c`, `p + i`, `i * 2^k`) are
// stepped too only when `i` then goes: read by nothing but its step and the latch's `i < n`, and the
// constants show a stepped local cannot wrap, the test reads that local and the step goes. What only
// some targets gain from is left to the backend. Whether it changed the function.
bool reduce_inductions( Function& func, const Type_table& types, Literal_pool& literals );
} // namespace keel
