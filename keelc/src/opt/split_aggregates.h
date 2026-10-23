// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ast/ast.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{
// After inline_drops: a struct or class local, past the parameters, that every place names through a
// field becomes one local per field named, repeated for nested fields; release only.
void split_aggregates( Function& func, const Ast& ast, Types& types );
} // namespace keel
