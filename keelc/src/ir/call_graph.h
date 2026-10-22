// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ir/kir.h"

namespace keel
{
// Whether `value` calls `callee`: a generic declaration lowers to one Function per instantiation.
inline bool calls( const Rvalue& value, const Function& callee )
{
    if( value.kind == Rvalue_kind::Call && value.callee == callee.declaration && value.type_arguments == callee.type_arguments )
    {
        return true;
    }

    return false;
}

// Indices into `functions`, grouped into the sets that call each other (Tarjan), each group after
// every group it calls into. A function calling nothing, or only itself, is a group of one.
std::vector<std::vector<u32>> callees_first( const std::vector<Function>& functions );
} // namespace keel