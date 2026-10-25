// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "ir/kir.h"
#include "parse/loader.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

// A checked module, as an object holds it: its source, its tree, and what resolving and checking
// its bodies found. A compile that loads it rebuilds the module's declarations with every other
// module's, and takes its bodies' results from here rather than checking them again.

namespace keel
{

struct Source_header
{
    std::string path;
    std::string text;
};

// A loaded module's results, in the loading compile's numbering.
struct Loaded_state
{
    File_id              file;
    std::vector<Node_id> declarations; // top-level, for the root

    // The resolver's: each use's declaration, and the overload chains of member scopes. Top-level
    // chains span modules, so the resolver rebuilds them.
    std::vector<std::pair<Node_id, Node_id>> bindings;
    std::vector<std::pair<Node_id, Node_id>> overloads;

    // The checker's.
    std::vector<std::pair<Node_id, Type_id>>              types;
    std::vector<std::pair<Node_id, Constant_value>>       constants;
    std::vector<std::pair<Node_id, Node_id>>              callees;
    std::vector<std::pair<Node_id, Type_id>>              through;
    std::vector<std::pair<Node_id, std::vector<Node_id>>> listed;
    std::vector<std::pair<Node_id, Instantiation>>        instantiations;
    std::vector<Generic_call>                             generic_calls;

    // The module's part as the dataflow checks left it: its functions, and the instances and
    // synthesised functions it holds.
    std::vector<Function> functions;
};

std::string write_state(
    const Program&            program,
    const Resolution&         resolution,
    const Types&              types,
    std::span<const Function> functions,
    File_id                   module,
    const Literal_pool&       literals,
    const Source_manager&     sm,
    const Interner&           interner
);

// Only the path and text, so the loader can add the file before reading the rest.
std::optional<Source_header> read_source( std::string_view state );

// Appends the module's nodes to the program's tree, as `file`, which read_source's text was added
// as and which the module graph has placed in its package. Its declared types are named in `table`
// before any is interned, so they read as the module's. Nothing when the state is malformed or
// names a module that is not loaded.
std::optional<Loaded_state> read_state(
    std::string_view      state,
    File_id               file,
    Program&              program,
    Interner&             interner,
    Literal_pool&         literals,
    Type_table&           table,
    const Source_manager& sm
);

} // namespace keel
