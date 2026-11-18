// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// In each round from -O2, within a block: a read of an element of a local array no pointer reaches,
// at an index the same local plus the same constant as an earlier store's, reads what that store
// wrote, and a store a later one to that element overwrites before any read goes. Whether it changed anything.
bool forward_elements( Function& func, const Type_table& types, const Literal_pool& literals );
} // namespace keel
