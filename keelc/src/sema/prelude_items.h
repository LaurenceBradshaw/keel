// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ast/node.h"

namespace keel
{

// The prelude declarations the compiler relies on by name, each invalid without a prelude.
struct Prelude_items
{
    Node_id str, result, box, from, ordering, location, hasher_write;
    Node_id equatable, comparable, hashable;
};

} // namespace keel
