// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"

namespace keel
{
// After elaborate_drops: a flagged drop of a local certainly full loses its flag, one certainly empty
// goes, and flag writes nothing reads go; release only.
void settle_drops( Function& func );
} // namespace keel
