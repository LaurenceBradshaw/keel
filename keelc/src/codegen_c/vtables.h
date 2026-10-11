// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ast/ast.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{

// One implementer seen through one interface: what a vtable holds, slot by slot.
struct Vtable
{
    Type_id              implementer {}; // the instance, `Circle` or `Box<i32>`
    Type_id              interface {};
    std::vector<Node_id> requirements; // the interface's methods, in slot order
    std::vector<Node_id> methods;      // the implementer's answer to each
};

// The type a place names, walked as the emitter walks its text.
Type_id place_type( const Ast& ast, Types& types, const Function& function, const Place& place );

// Every implementer the functions convert to an interface pointer, each pair once, in the order first met.
std::vector<Vtable> vtables_needed( const Ast& ast, Types& types, const std::vector<Function>& functions );

} // namespace keel
