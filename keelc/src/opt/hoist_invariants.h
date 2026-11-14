// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <vector>
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "ir/loops.h"

namespace keel
{
// In the loop phase, innermost loop first: a statement whose operands the loop never changes, into a
// local written only there and read in the loop only after it, moves to the preheader; so does such a
// read through a pointer inside any statement, into a new local; so does a bounds check the loop's
// test does not cover, as one range check. Under `pin_named` a named local's statement stays, and what
// moves takes the loop test's span. Whether it changed the function, normalising the loops included.
bool hoist_invariants( Function& func, const Literal_pool& literals, bool pin_named );

// A read through a pointer whose place names no local the loop writes.
struct Blocked_read
{
    Place   place;
    Type_id type;
    bool    early;   // somewhere it runs on the first turn with only stores before it
    bool    reached; // a store in the loop may change it
};
// For an innermost loop that stores, and only through pointers, with no call, drop or release: each
// read that stores alone may keep in it.
std::optional<std::vector<Blocked_read>> blocked_reads( const Function& func, const Loops& found, u32 index );
} // namespace keel
