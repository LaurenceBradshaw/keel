// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{

// A declaration on one line as its source writes it, without its body or initialiser:
// `public void push( move T value )`. A type parameter `bindings` binds reads as its argument, and
// its `where` clause is left out: `public void push( move i32 value )`. Given `sm`, a parameter's
// default is written too (D79).
std::string print_signature(
    const Ast&            ast,
    const Resolution&     resolution,
    const Types&          types,
    const Interner&       interner,
    Node_id               decl,
    const Bindings&       bindings = {},
    const Source_manager* sm       = nullptr
);

// D77: a field's signature with ` = value` as written, since a literal or constructor may rely on it.
std::string with_default( const Ast& ast, const Source_manager& sm, Node_id decl, std::string signature );

// A declaration's `///` lines without their markers, or empty when it has none.
std::string documentation( const Ast& ast, const Source_manager& sm, Node_id decl );

// The section a `/// --- Name ---` banner before the declaration opens, or empty when none does.
std::string opened_section( const Ast& ast, const Source_manager& sm, Node_id decl );

} // namespace keel
