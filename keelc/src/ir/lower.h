// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <vector>
#include "ast/ast.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "ir/kir.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{

// The loaded modules' functions as given, then a Function per Function_decl, in declaration order,
// then one per instance, then a destructor per closed owning enum. None of the given is lowered again.
std::vector<Function>
// Types is mutable only for its table: lowering interns types the checker never named - `T*` as
// `i32*` in an instance, `i32*` for a borrowed argument. Nothing the checker recorded changes.
lower(
    const Ast&            ast,
    const Resolution&     resolution,
    Types&                types,
    Literal_pool&         literals,
    const Source_manager& sm,
    bool                  overflow_checks,
    std::vector<Function> loaded = {}
);

// Whether `functions` holds `declaration` at `type_arguments`.
bool provided( std::span<const Function> functions, Node_id declaration, std::span<const Type_id> type_arguments );

} // namespace keel