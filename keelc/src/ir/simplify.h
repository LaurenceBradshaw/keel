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
// `statement` put at `at` in `block`, whose statements move to the end of `func.statements`; the old
// run stays as a gap until `remove_statements`.
void insert_statement( Function& func, Block_id block, u32 at, Statement statement );
// Drops every block that block 0 cannot reach, and renumbers the rest.
void remove_unreachable_blocks( Function& func );
// Drops every local beyond the signature that only storage markers name, with those markers, and
// renumbers the rest; `pin_named` keeps every named local.
void remove_unused_locals( Function& func, bool pin_named );
} // namespace keel
