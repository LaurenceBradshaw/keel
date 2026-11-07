// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <vector>
#include "ast/ast.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{

// What one call or drop may do to the owning globals, through the functions it may reach.
struct Call_effects
{
    Rvalue_id            call {};
    std::vector<Node_id> writes;
    std::vector<Node_id> returns_into;
};

// D80: what a function may do to the owning globals, through any call. Each list is sorted.
struct Global_effects
{
    std::vector<Node_id>      writes;       // assigned, dropped, or lent other than to read
    std::vector<Node_id>      returns_into; // its returned address may point into the global
    std::vector<Node_id>      owning;       // the owning globals it names, or its calls reach
    std::vector<Call_effects> calls;        // by `call`; only those with an effect

    // Null when the call has no effect.
    const Call_effects* of( const Function& function, const Rvalue& call ) const;

    bool owns( Node_id global ) const;
};

// One per function. An indirect call reaches every function whose address is taken, and an
// interface call every method answering its requirement; an `extern` writes nothing.
std::vector<Global_effects> global_effects_of( std::span<const Function> functions, const Ast& ast, Types& types );

} // namespace keel
