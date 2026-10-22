// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"

namespace keel
{
// A block reached only by a `goto` joins the block that jumps to it: its statements follow that
// block's, and its terminator replaces the `goto`.
void merge_blocks( Function& func );
} // namespace keel
