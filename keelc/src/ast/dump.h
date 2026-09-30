// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "ast/ast.h"
#include "common/interner.h"
#include "common/source_manager.h"

#include <iosfwd>

namespace keel
{

// Renders the tree from ast.root() as indented text, for --dump-ast and the tests/parse goldens.
// Prints nothing when the root is invalid.
void dump_ast( const Ast& ast, const Source_manager& sm, const Interner& interner, std::ostream& out );

} // namespace keel
