// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ast/ast.h"
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{

// A Function per Function_decl, in declaration order, then one per instance, then a destructor
// per closed owning enum.
std::vector<Function>
// Types is mutable only for its table: lowering interns types the checker never named - `T*` as
// `i32*` in an instance, `i32*` for a borrowed argument. Nothing the checker recorded changes.
lower( const Ast& ast, const Resolution& resolution, Types& types, Literal_pool& literals );

} // namespace keel