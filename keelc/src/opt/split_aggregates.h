// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ast/ast.h"
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{
// After inline_drops: a struct or class local, a parameter with a field written, or the return slot
// with a field read, that every place names through a field becomes one local per field named,
// repeated for nested fields; so does an enum local every place names through its tag or a payload
// field, and a sized array local every place indexes by a known constant; release only.
void split_aggregates( Function& func, const Ast& ast, Types& types, const Literal_pool& literals );
} // namespace keel
