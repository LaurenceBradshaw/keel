#pragma once
#include "common/literal_pool.h"
#include "ir/kir.h"

namespace keel
{

// What a flag is made of, none of which the Function carries. Passed in as data rather than as a
// Type_table and a Literal_pool to look them up in, so the pass stays a function of its inputs.
struct Flag_vocabulary
{
    Type_id    bool_type {};
    Literal_id false_literal {};
    Literal_id true_literal {};
};

// PLAN §7. A local moved on only some paths cannot be dropped unconditionally, so it gets a hidden
// bool: false while its storage is empty, true once assigned, false again once moved, tested at the
// drop. Exact on every path *by construction*, because it records what happened rather than what
// might have - which is why this needs no dataflow, unlike move_check's diagnostics.
void elaborate_drops( Function& func, const Flag_vocabulary& vocabulary );

} // namespace keel
