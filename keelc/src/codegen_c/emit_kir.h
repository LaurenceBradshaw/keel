// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "ast/ast.h"
#include "common/imports.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{

// The prelude's C is a file of its own, which the program's includes: the prelude sees nothing of
// the program, so its half compiles alone.
enum class C_part : u8
{
    Program,
    Prelude,
};

// `include` names the prelude's file, written after the standard headers; empty writes none.
// Types is mutable only for its table, which a struct reached only through a field grows.
std::string emit_c_from_kir(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    Types&                       types,
    const Literal_pool&          literals,
    const Source_manager&        sm,
    const Interner&              interner,
    const Imports&               imports = {},
    C_part                       part    = C_part::Program,
    std::string_view             include = {}
);

} // namespace keel