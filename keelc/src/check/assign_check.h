// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "common/span.h"
#include "ir/kir.h"

namespace keel
{

// An obligation a path can leave the function without meeting: an `out` parameter (D31) or the
// return slot (D9). Spans only; check/report does the wording.
struct Unassigned_error
{
    Local_id local {};
    Node_id  field {};      // a constructor's field, when that is what is owed
    Span     at {};         // the `return` it can reach unassigned
    bool     maybe = false; // unassigned on some paths into here, not all
};

// D9: a read of a local that may hold no value yet.
struct Uninitialised_read
{
    Local_id local {};
    Node_id  field {};      // a constructor's field, when that is what was read
    Span     at {};         // the read
    bool     maybe = false; // assigned on some paths into here, not all
    bool     whole = false; // `this` used whole, so every field is read
};

// A constructor writing an owning field or `const` field that may already hold a value: the old one would
// need a drop decided per path, so it is refused instead.
struct Reassigned_field
{
    Node_id field {};
    Span    at {};
    bool    maybe    = false; // holds a value on some paths into here, not all
    bool    is_const = false; // refused for being const rather than for owning
};

struct Assignment_report
{
    std::vector<Unassigned_error>   unassigned;
    std::vector<Uninitialised_read> reads;
    std::vector<Reassigned_field>   reassigned;
    std::vector<Span>               diverging_returns; // a reachable Return in a `never` function
};

// D9 and D31: definite assignment, a *must* analysis over the CFG check_moves walks. A pass of its
// own because check_moves joins Uninitialised with Live to Live, the opposite answer.
Assignment_report check_assignment( const Function& func );

} // namespace keel