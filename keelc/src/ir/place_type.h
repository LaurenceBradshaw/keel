// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ast/ast.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{

// The type a place names, walked as the emitter walks its text.
Type_id place_type( const Ast& ast, Types& types, const Function& function, const Place& place );

} // namespace keel
