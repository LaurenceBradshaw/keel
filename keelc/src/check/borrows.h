// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "common/span.h"
#include "common/types.h"
#include "ir/kir.h"

// What each pointer local borrows, shared by the move check's one-call rule and the loans pass.

namespace keel
{

// What a pointer local borrows: a local's place (`of`), or what a call returned (`through`), never
// both. Valid only for one assigned exactly once, as a borrowed argument's temporary is, so the
// answer needs no flow.
struct Borrow
{
    Local_id        of {};
    Span            at {};
    u32             assignments = 0;
    Place           place {};                          // what is borrowed; only when `of` is valid
    Address_purpose purpose = Address_purpose::Borrow; // what the borrow is for
    const Rvalue*   through = nullptr;                 // the call that returned this address
    bool            bound   = false;                   // assigned an address itself, as a `ref` binding is and a pointer never
};

// One per local, indexed by Local_id.
std::vector<Borrow> borrows_of( const Function& func );

// One place is a prefix of the other: `p` overlaps `p.b`, and `p.a` does not.
bool overlaps( const Function& func, const Place& a, const Place& b );

// D54: a call's returned address borrows each of its borrowed arguments, the receiver included.
// Ends because `through` names an earlier statement, assigned once.
bool reaches( const Function& func, const std::vector<Borrow>& borrows, const Borrow& element, const Place& whole );

} // namespace keel
