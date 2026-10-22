// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"

namespace keel
{
// After propagate_constants: a read of a local holding a constant or a copy of a local reads that
// instead, leaving the write to remove_dead_stores; release only.
void propagate_values( Function& func );
} // namespace keel
