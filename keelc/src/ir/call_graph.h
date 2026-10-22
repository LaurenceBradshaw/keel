// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ir/kir.h"

namespace keel
{
// Whether `statement` calls `callee`: a generic declaration lowers to one Function per instantiation.
// A drop calls its destructor.
inline bool calls( const Statement& statement, const Function& callee )
{
    const bool call = statement.kind == Statement_kind::Assign && statement.value.kind == Rvalue_kind::Call;
    const bool drop = statement.kind == Statement_kind::Drop && statement.value.callee.is_valid();

    return ( call || drop ) && statement.value.callee == callee.declaration &&
           statement.value.type_arguments == callee.type_arguments;
}

// Indices into `functions`, grouped into the sets that call each other (Tarjan), each group after
// every group it calls into. A function calling nothing, or only itself, is a group of one.
std::vector<std::vector<u32>> callees_first( const std::vector<Function>& functions );
} // namespace keel