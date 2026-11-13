// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "ir/purity.h"

namespace keel
{
// In -O3's loop phase: a pure computation every path to it already made, into a local that still
// holds it, copies that local. Exact repeats only, so a float is never reassociated; a read through
// a pointer, of a global or of an addressed local is forgotten at `clobbers`. A call of a pure
// function is one, keyed by its callee and its arguments. Whether it changed anything.
bool eliminate_common( Function& func, const Literal_pool& literals, const Purity& purity );
} // namespace keel
