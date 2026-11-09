// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <string>
#include <vector>
#include "codegen/description.h"
#include "codegen/parts.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "ir/kir.h"
#include "parse/module_graph.h"
#include "sema/type_checker.h"

namespace keel
{

struct Emitted
{
    std::string              c;
    std::vector<std::string> defined; // every symbol the C defines with external linkage, for an object's index
};

// `part`'s C, complete without any other part's; a null `parts` writes the whole compile as one.
// Types is mutable only for its table, which a symbol's substituted parameters may grow.
Emitted emit_c_from_kir(
    const std::vector<Function>& functions,
    const Description&           program,
    Types&                       types,
    const Literal_pool&          literals,
    const Source_manager&        sm,
    const Interner&              interner,
    const Module_graph&          modules = {},
    const Parts*                 parts   = nullptr,
    File_id                      part    = {},
    bool                         object  = false
);

} // namespace keel