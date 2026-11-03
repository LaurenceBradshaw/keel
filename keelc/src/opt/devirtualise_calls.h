// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ast/ast.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{
// A requirement called through `_p`, assigned once `&x` or `copy q as Shape*` of a concrete
// implementer and used only as such receivers, calls the implementer's method directly, `_p` retyped
// to point at it; release only. Whether anything changed.
bool devirtualise_calls( Function& func, const Ast& ast, Types& types );

// From each function to the methods of every implementer it converts to an interface, which a call
// made direct may reach: for `callees_first`, so they are optimised first.
std::vector<std::vector<u32>> dispatch_edges( const std::vector<Function>& functions, const Ast& ast, Types& types );
} // namespace keel
