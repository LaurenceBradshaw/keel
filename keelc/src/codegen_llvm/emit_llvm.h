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

struct Llvm_options
{
    std::filesystem::path object {};       // written there unless empty
    bool                  print = false;   // into Llvm_output::ir
    const Parts*          parts = nullptr; // null emits the whole compile as one module
    File_id               part {};         // else the part this module is
    bool                  klo   = false;   // an instance is a COMDAT, as several objects may define it
    bool                  debug = false;   // a named local stays in memory, for a debugger
};

// The compile as one LLVM module. Types is mutable only for its table.
Llvm_output emit_llvm(
    const std::vector<Function>& functions,
    const Description&           program,
    Types&                       types,
    const Literal_pool&          literals,
    const Source_manager&        sm,
    const Interner&              interner,
    const Module_graph&          modules,
    const Llvm_options&          options
);

} // namespace keel
