// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"

namespace keel
{
void simplify( Function& func, const Literal_pool& literals );

// Every `debug` branch condition becomes the constant `value`: true under --debug, false otherwise.
void settle_debug( Function& func, Literal_id value );

// Drops each statement `deleted` marks, indexed like `func.statements`, and closes the gaps.
void remove_statements( Function& func, const std::vector<bool>& deleted );
} // namespace keel
