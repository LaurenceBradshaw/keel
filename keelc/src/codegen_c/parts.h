// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <unordered_map>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "ir/kir.h"
#include "parse/module_graph.h"
#include "sema/type_checker.h"

namespace keel
{

// An instance or a synthesised function: placed by which module reaches it, so several may define it.
// A module's build and drop functions are its own.
inline bool placed_by_reach( const Function& function )
{
    return !function.globals.is_valid() && ( !function.type_arguments.empty() || !function.declaration.is_valid() );
}

// Which module's C file each function goes in (D71). Every part compiles alone.
//
//   a function declared at no type arguments   its declaration's module
//   a build or drop function                   its globals' module
//   an instance or a synthesised function      the first module, dependencies first, whose own
//                                              functions reach it; the input's when none does
//   one of the first `provided`, loaded        the module whose object held it
//
// Types belong to no part: each writes every type it names. The emitter, --dump-kir and the test
// fixture all ask here.
class Parts
{
public:
    Parts(
        const Ast&                   ast,
        Types&                       types,
        const Module_graph&          modules,
        File_id                      input,
        const std::vector<Function>& functions,
        std::span<const File_id>     provided = {}
    );

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

// A release program build's sweep: drops every function that neither the input's `main` nor a build
// or drop function reaches through calls, `&f`, drops, vtables or a global's initialiser, so their
// types and vtables go too. Keeps
// `provided` in step; a program without `main` keeps everything.
void remove_unreached(
    std::vector<Function>& functions,
    std::vector<File_id>&  provided,
    const Ast&             ast,
    Types&                 types,
    const Module_graph&    modules,
    File_id                input,
    const Interner&        interner
);

} // namespace keel
