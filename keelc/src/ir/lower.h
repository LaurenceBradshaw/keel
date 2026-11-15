// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>
#include "ast/ast.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "ir/instances.h"
#include "ir/kir.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{

// Types is mutable only for its table: lowering interns types the checker never named - `T*` as
// `i32*` in an instance, `i32*` for a borrowed argument. Nothing the checker recorded changes.
class Lowerer
{
public:
    Lowerer(
        const Ast&            ast,
        const Resolution&     resolution,
        Types&                types,
        Literal_pool&         literals,
        const Source_manager& sm,
        const Interner&       interner,
        bool                  overflow_checks
    );

    // The loaded modules' functions as given, then a Function per Function_decl, in declaration order,
    // each followed by its lambdas, then one per instance, each followed by its lambdas, then a destructor
    // per closed owning enum. None of the given is lowered again.
    std::vector<Function> lower( std::vector<Function> loaded = {} );

    // After lower(): `instance` and what it reaches that `functions` lacks, appended. Its index.
    u32 instantiate( std::vector<Function>& functions, Instantiation instance );

private:
    std::vector<Function> lambdas( Node_id declaration, const Bindings& bindings ) const;
    void                  drain( std::vector<Function>& functions );

    const Ast&                    ast_;
    const Resolution&             resolution_;
    Types&                        types_;
    Literal_pool&                 literals_;
    const Source_manager&         sm_;
    const Interner&               interner_;
    bool                          overflow_checks_;
    std::optional<Instance_queue> queue_ {}; // made by lower(), after the plain functions
    std::size_t                   given_ = 0;
    std::unordered_set<u64>       dropped_ {}; // the types drops were made for, as append_synthesised_drops keys them
};

// Whether `functions` holds `declaration` at `type_arguments`.
bool provided(
    std::span<const Function> functions,
    Node_id                   declaration,
    std::span<const Type_id>  type_arguments,
    Token_kind                compares = Token_kind::Unknown
);

} // namespace keel