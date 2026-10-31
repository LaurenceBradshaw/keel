// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{

// D54: what is wrong with one call's arguments, when the error is about them rather than the flow.
enum class Call_conflict : u8
{
    None,
    Moved_and_borrowed, // moved into the call at `moved` that also borrows it at `use`
    Out_twice,          // `use` and `other` overlap, and both are `out`
    Element_and_whole,  // `use` reaches inside what `other` lets the call change
    Aliased             // `use` and `other` are one object, and the call could change it through either, or `out` destroys it
};

// A read after a move, or a call's conflicting arguments. Spans only; check/report does the wording.
struct Move_error
{
    Local_id      local {};
    u32           field = k_no_field; // into Function::fields, when a field is what was moved
    Span          use {};             // the read
    Span          moved {};           // the move that killed it
    bool          maybe    = false;   // moved on some paths, not all; for a one-call error, indices that may differ
    Call_conflict conflict = Call_conflict::None;
    Span          other {}; // the call's other argument, for the one-call errors
};

// PLAN §8. Flow-sensitive, per field, over the CFG. `owning` is check/borrow_check's
// owning_locals: only an owner can be freed under an argument that aliases it. `addressing` is its
// addressing_locals: only a call result that can hold an address may reach into an argument.
std::vector<Move_error> check_moves(
    const Function& func, const std::vector<bool>& owning, const std::vector<bool>& addressing, const Literal_pool& literals
);

} // namespace keel