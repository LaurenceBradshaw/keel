// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include "ast/ast.h"
#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "parse/module_graph.h"
#include "prelude/prelude.h"

namespace keel
{

struct Program
{
    Ast          ast;
    Module_graph modules;

    std::unordered_map<u32, std::string> package_docs; // by package symbol, the program's own under Symbol_id {}
    std::unordered_map<u32, u32>         first_node;   // by file: its nodes run from here, unbroken
};

struct Package
{
    std::string           name;
    std::filesystem::path root;
};

// Lexes and parses `input` and every module it imports, transitively, into one tree whose root holds
// every file's declarations. A module is `name.kl` beside the input file, and each is loaded once.
Program load_program(
    File_id                  input,
    Source_manager&          sm,
    Interner&                interner,
    Literal_pool&            literals,
    Diagnostics&             diags,
    std::span<const Package> packages = {},
    std::string_view         prelude  = prelude_source()
);

// Why `name` cannot be given to `--package`, or nothing when it can.
std::optional<std::string> package_name_error( std::string_view name, const Interner& interner );

} // namespace keel
