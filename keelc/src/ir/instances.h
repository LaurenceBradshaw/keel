// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ast/ast.h"
#include "sema/type.h"
#include "sema/type_checker.h"

namespace keel
{

// Monomorphisation: every closed instance of a generic the program reaches, each once. Types is
// mutable because substituting an edge's arguments can intern a type that does not exist yet.
std::vector<Instantiation> instances_to_emit( const Ast& ast, Types& types );

// The instance's type arguments bound to its declaration's parameters, positionally.
Bindings bindings_for( const Ast& ast, const Types& types, const Instantiation& instance );

} // namespace keel
