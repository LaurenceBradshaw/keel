// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"

namespace keel
{
// Merges the two locals of `a = move b` where their values are never alive at once; release only.
void coalesce_moves( Function& func );
} // namespace keel