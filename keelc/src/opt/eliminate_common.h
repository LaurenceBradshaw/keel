// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"

namespace keel
{
// In -O3's loop phase: a pure computation every path to it already made, into a local that still
// holds it, copies that local. Exact repeats only, so a float is never reassociated; a read through
// a pointer, of a global or of an addressed local is forgotten at `clobbers`. Whether it changed anything.
bool eliminate_common( Function& func, const Literal_pool& literals );
} // namespace keel
