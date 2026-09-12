#pragma once
#include <vector>
#include "common/span.h"
#include "ir/kir.h"

namespace keel
{

// An obligation a path can reach a way out of the function without meeting: an `out` parameter
// (D31) or the return slot (D9). Carries a span rather than a message, so the pass needs no
// Interner and no Diagnostics - the driver has both.
struct Unassigned_error
{
    Local_id local {};
    Span     at {};         // the `return` it can reach unassigned
    bool     maybe = false; // unassigned on some paths into here, not all
};

// D9: a read of a local that may hold no value yet. The other half of the same question - one asks
// whether a value exists by the time the function leaves, the other whether it exists by the time
// something looks at it - so both come out of one walk.
struct Uninitialised_read
{
    Local_id local {};
    Span     at {};         // the read
    bool     maybe = false; // assigned on some paths into here, not all
};

struct Assignment_report
{
    std::vector<Unassigned_error>   unassigned;
    std::vector<Uninitialised_read> reads;
};

// PLAN D9 and D31. Definite assignment, over the same CFG check_moves walks and with the opposite
// lattice: this is a *must* analysis, so blocks start "assigned" and merging lowers them, where
// §8's move lattice starts empty and merging raises them. That difference is why it is a second
// pass rather than a second question inside the first - and it is also why D9 lives here rather
// than beside `move`: check_moves' Uninitialised is a lattice bottom meaning "no information yet",
// so joining it with Live gives Live, which is the opposite of what D9 has to conclude.
Assignment_report check_assignment( const Function& func );

} // namespace keel