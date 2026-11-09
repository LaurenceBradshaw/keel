// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <filesystem>
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

struct Llvm_output
{
    std::string error; // what could not be emitted, a whole sentence; empty when it all was
    std::string ir;    // the module as text, when asked for

    std::vector<std::string> defined; // every symbol the object defines with external linkage, for a .klo's index
};

// `part` as one LLVM module, or the whole compile when `parts` is null: written as an object to
// `object` unless it is empty, and printed into `ir` when `print` is set. In a .klo (`klo`) an
// instance is a COMDAT, as several objects may define it. Types is mutable only for its table.
Llvm_output emit_llvm(
    const std::vector<Function>& functions,
    const Description&           program,
    Types&                       types,
    const Literal_pool&          literals,
    const Source_manager&        sm,
    const Interner&              interner,
    const Module_graph&          modules,
    const std::filesystem::path& object,
    bool                         print = false,
    const Parts*                 parts = nullptr,
    File_id                      part  = {},
    bool                         klo   = false
);

} // namespace keel
