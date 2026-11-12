// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"

namespace keel
{
// After elaborate_drops, so every drop's read is explicit: a write to a local no path reads again
// goes, unless `pin_named` keeps a named local's.
void remove_dead_stores( Function& func, bool pin_named );
} // namespace keel
