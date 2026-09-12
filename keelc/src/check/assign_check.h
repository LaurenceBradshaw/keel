#pragma once
#include <vector>
#include "common/span.h"
#include "ir/kir.h"

namespace keel
{

// An `out` parameter a path can reach the end of the function without assigning. Carries a span
// rather than a message, so the pass needs no Interner and no Diagnostics - the driver has both.
struct Unassigned_error
{
    Local_id local {};
    Span     at {};         // the `return` it can reach unassigned
    bool     maybe = false; // unassigned on some paths into here, not all
};

// PLAN D31. Definite assignment for `out` parameters, over the same CFG check_moves walks and with
// the opposite lattice: this is a *must* analysis, so blocks start "assigned" and merging lowers
// them, where §8's move lattice starts empty and merging raises them. That difference is why it is
// a second pass rather than a second question inside the first.
std::vector<Unassigned_error> check_assignment( const Function& func );

} // namespace keel