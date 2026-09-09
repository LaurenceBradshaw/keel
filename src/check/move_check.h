#pragma once
#include <vector>
#include "ir/kir.h"

namespace keel
{

// Where a value was read after it was moved. Carries spans rather than a message, so the pass
// needs no Interner and no Diagnostics - the driver has both and does the wording.
struct Move_error
{
    Local_id local {};
    Span     use {};        // the read
    Span     moved {};      // the move that killed it
    bool     maybe = false; // moved on some paths into here, not all
};

// PLAN §8. Flow-sensitive, per local, over the CFG. Takes nothing but the function: everything it
// reports with is already in there.
std::vector<Move_error> check_moves( const Function& func );

} // namespace keel