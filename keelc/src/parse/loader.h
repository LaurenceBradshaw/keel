// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <filesystem>
#include <span>
#include <string_view>
#include "ast/ast.h"
#include "common/diagnostics.h"
#include "common/imports.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "prelude/prelude.h"

namespace keel
{

struct Program
{
    Ast     ast;
    Imports imports;
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

} // namespace keel
