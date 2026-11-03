// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <utility>
#include "ast/ast.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{

// The type a place names, walked as the emitter walks its text.
Type_id place_type( const Ast& ast, Types& types, const Function& function, const Place& place );

// The implementer and interface of a conversion to an interface pointer or `box<Shape>`, or neither
// when the value is one already: `&(*s)` with `s` a `Shape*` is `s` again, and needs no table.
std::pair<Type_id, Type_id> interface_conversion( const Ast& ast, Types& types, const Function& function, const Rvalue& value );

} // namespace keel
