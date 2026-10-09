// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ir/kir.h"

namespace keel
{

// D54: what is wrong with one call's arguments, when the error is about them rather than the flow.
enum class Call_conflict : u8
{
    None,
    Moved_and_borrowed, // moved into the call at `moved` that also borrows it at `use`
    Out_twice,          // `use` and `other` overlap, and both are `out`
    Element_and_whole,  // `use` reaches inside what `other` lets the call change
    Aliased             // `use` and `other` are one object, and the call could change it through either
};

// A read after a move, or a call's conflicting arguments. Spans only; check/report does the wording.
struct Move_error
{
    Local_id      local {};
    Span          use {};           // the read
    Span          moved {};         // the move that killed it
    bool          maybe    = false; // moved on some paths into here, not all
    Call_conflict conflict = Call_conflict::None;
    Span          other {}; // the call's other argument, for the one-call errors
};

// PLAN §8. Flow-sensitive, per local, over the CFG. `owning` is check/borrow_check's
// owning_locals: only an owner can be freed under an argument that aliases it.
std::vector<Move_error> check_moves( const Function& func, const std::vector<bool>& owning );

} // namespace keel