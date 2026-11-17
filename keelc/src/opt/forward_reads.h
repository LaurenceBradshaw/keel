// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/initialised.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// In the loop phase: a read of a place through a pointer, where every path to it copied the place to a
// local that still holds it, reads that local; a call, a drop, or a store through a pointer or to an
// addressed local between them keeps the read, but a store of one scalar kind keeps no read of another
// (D89). A read in a loop inside a larger rvalue or a terminator, of a place read elsewhere too, is first read
// into a local of its own, so a later read can take it. Whether it changed anything.
bool forward_reads( Function& func, const Type_table& table );

// Shared with the other loop passes.
// Whether `place` is rooted at `local` or indexes by it.
bool mentions( const Function& func, const Place& place, Local_id local );
// Whether `statement` or `terminator` reads `local`; a write of the whole local is not a read, and a
// drop reads its place and its flag.
bool reads( const Function& func, const Statement& statement, Local_id local );
bool reads( const Function& func, const Terminator& terminator, Local_id local );
// Locals a pointer reaches. `&(*p)` is not one of `p`'s, unlike `addressed_locals`; `&init` keeps no
// pointer past its call.
Local_set addressed_locals_of( const Function& func );
bool      is_addressed( const Local_set& addressed, Local_id local );
// Whether `statement` may change what a pointer reaches: a call, a drop, or a store through a pointer,
// to a global, or to a local a pointer reaches.
bool clobbers( const Function& func, const Local_set& addressed, const Statement& statement );
// Whether memory a store of `stored` writes is never read as `read` (D89): each is a number, `bool` or
// a thin pointer, of different kinds.
bool distinct_memory( const Type_table& table, Type_id stored, Type_id read );
} // namespace keel
