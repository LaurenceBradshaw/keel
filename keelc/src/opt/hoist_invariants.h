// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"

namespace keel
{
// In -O3's loop phase, innermost loop first: a statement whose operands the loop never changes, into a
// local written only there and read in the loop only after it, moves to the preheader; so does such a
// read through a pointer inside any statement, into a new local. Whether it changed the function,
// normalising the loops included.
bool hoist_invariants( Function& func );
} // namespace keel
