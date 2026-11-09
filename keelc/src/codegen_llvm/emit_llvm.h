// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "codegen/description.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "ir/kir.h"
#include "parse/module_graph.h"
#include "sema/type_checker.h"

namespace keel
{

struct Llvm_output
{
    std::string error; // what could not be emitted, a whole sentence; empty when it all was
    std::string ir;    // the module as text, when asked for
};

// The whole compile as one LLVM module: written as an object to `object` unless it is empty, and
// printed into `ir` when `print` is set. Types is mutable only for its table, as the C emitter's is.
Llvm_output emit_llvm(
    const std::vector<Function>& functions,
    const Description&           program,
    Types&                       types,
    const Literal_pool&          literals,
    const Source_manager&        sm,
    const Interner&              interner,
    const Module_graph&          modules,
    const std::filesystem::path& object,
    bool                         print = false
);

} // namespace keel
