// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/initialised.h"
#include "ir/kir.h"

namespace keel
{
// In -O3's loop phase: a read of a place through a pointer, where every path to it copied the place to a
// local that still holds it, reads that local; a call, a drop, or a store through a pointer or to an
// addressed local between them keeps the read. Whether it changed anything.
bool forward_reads( Function& func );

// Shared with hoist_invariants.
// Whether `place` is rooted at `local` or indexes by it.
bool mentions( const Function& func, const Place& place, Local_id local );
// Locals a pointer reaches. `&(*p)` is not one of `p`'s, unlike `addressed_locals`; `&init` keeps no
// pointer past its call.
Local_set addressed_locals_of( const Function& func );
bool      is_addressed( const Local_set& addressed, Local_id local );
// Whether `statement` may change what a pointer reaches: a call, a drop, or a store through a pointer,
// to a global, or to a local a pointer reaches.
bool clobbers( const Function& func, const Local_set& addressed, const Statement& statement );
} // namespace keel
