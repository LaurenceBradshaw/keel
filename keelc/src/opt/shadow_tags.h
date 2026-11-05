// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// After split_aggregates: a tagged local that no address reaches whole or at its tag reads its tag
// from a scalar local, written beside each write, so SCCP sees it; release only.
void shadow_tags( Function& func, const Type_table& types );
} // namespace keel
