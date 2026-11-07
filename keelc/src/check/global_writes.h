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

// D80: what a function may do to the owning globals, through any call. Each list is sorted.
struct Global_effects
{
    std::vector<Node_id> writes;       // assigned, dropped, or lent other than to read
    std::vector<Node_id> returns_into; // its returned address may point into storage the global owns
};

// One per function. An indirect call reaches every function whose address is taken, and an
// interface call every method answering its requirement; an `extern` writes nothing.
std::vector<Global_effects> global_effects_of( std::span<const Function> functions, const Ast& ast, Types& types );

} // namespace keel
