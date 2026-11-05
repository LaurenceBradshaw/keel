// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"

namespace keel
{
// After split_aggregates: a tag of a local or of a field under it, that no address reaches, is read from
// a scalar local written beside each write, so SCCP sees it; release only.
void shadow_tags( Function& func );
} // namespace keel
