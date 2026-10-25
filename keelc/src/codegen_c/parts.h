// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <unordered_map>
#include <vector>
#include "ast/ast.h"
#include "ir/kir.h"
#include "parse/module_graph.h"
#include "sema/type_checker.h"

namespace keel
{

// Which module's C file each function goes in (D71). Every part compiles alone.
//
//   a function declared at no type arguments   its declaration's module
//   an instance or a synthesised function      the first module, dependencies first, whose own
//                                              functions reach it; the input's when none does
//
// Types belong to no part: each writes every type it names. The emitter, --dump-kir and the test
// fixture all ask here.
class Parts
{
public:
    Parts( const Ast& ast, Types& types, const Module_graph& modules, File_id input, const std::vector<Function>& functions );

    File_id of( const Function& function ) const;
    File_id of( Node_id declaration ) const;

    // Indices into the functions: what `function` calls, drops, or puts in a vtable.
    std::span<const u32> reaches( u32 function ) const;

    std::span<const File_id> order() const;
    File_id                  input() const;

private:
    // The function `declaration` lowers to at `type_arguments`, or k_invalid_kir_id.
    u32 find( Node_id declaration, std::span<const Type_id> type_arguments ) const;

    const Ast&                        ast_;
    const std::vector<Function>&      functions_;
    std::vector<File_id>              order_;
    std::unordered_multimap<u32, u32> by_declaration_;
    std::vector<std::vector<u32>>     reaches_;
    std::vector<File_id>              owners_;
};

} // namespace keel
