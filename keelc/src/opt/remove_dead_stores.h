// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "ir/purity.h"
#include "sema/type.h"

namespace keel
{
// After elaborate_drops, so every drop's read is explicit: a write to a local no path reads again
// goes, unless `pin_named` keeps a named local's. An unread call goes only if `purity` finds it removable.
// First, a store of a constant into a small local array whose element(s) it may write already hold it.
void remove_dead_stores(
    Function& func, bool pin_named, const Purity& purity, const Type_table& types, const Literal_pool& literals
);
} // namespace keel
