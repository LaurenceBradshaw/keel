// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <span>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "parse/module_graph.h"
#include "sema/type_checker.h"

namespace keel
{
// The program's entry: `main`'s arguments as `str`s when it takes them, every module's globals built
// dependencies first, the Keel `main` called, and the globals dropped in reverse. None when `input`
// declares no `main`.
std::optional<Function> main_shim(
    const Ast&                ast,
    Types&                    types,
    Literal_pool&             literals,
    const Interner&           interner,
    const Module_graph&       modules,
    File_id                   input,
    std::span<const Function> functions
);
} // namespace keel
