// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ast/ast.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// §6.7: each call of a requirement through an interface pointer `s` becomes a read of `s`'s vtable
// slot and of its object, and an indirect call of the one with the other; after the checks and the
// optimiser, which read the receiver call.
void lower_dispatch( Function& func, const Ast& ast, Type_table& table );
} // namespace keel
