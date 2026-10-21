// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"

namespace keel
{
// After elaborate_drops: a flagged drop of a local certainly full loses its flag, and one certainly
// empty goes; release only. The flag writes this leaves unread go in remove_dead_stores.
void settle_drops( Function& func );
} // namespace keel
