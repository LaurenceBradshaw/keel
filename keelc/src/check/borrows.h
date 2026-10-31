// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <vector>
#include "common/literal_pool.h"
#include "common/span.h"
#include "common/types.h"
#include "ir/kir.h"

// What each pointer local borrows, shared by the move check's one-call rule and the loans pass.

namespace keel
{

// What a pointer local borrows: a local's place (`of`), or what a call returned (`through`), never
// both. Valid only for one assigned exactly once, as a borrowed argument's temporary is, so the
// answer needs no flow, or for a chosen one (D75), which holds one of its `choices`.
struct Borrow
{
    Local_id              of {};
    Span                  at {};
    u32                   assignments = 0;
    Place                 place {};                          // what is borrowed; only when `of` is valid
    Address_purpose       purpose = Address_purpose::Borrow; // what the borrow is for
    const Rvalue*         through = nullptr;                 // the call that returned this address
    bool                  bound   = false; // assigned an address itself, as a `ref` binding is and a pointer never
    Local_id              via {};          // the pointer `&(*r).f` was taken through, `r`
    std::optional<u64>    constant {};     // the integer it always holds: assigned once from a constant, never addressed
    Local_id              copy_of {};      // the local it copies whole: assigned once, from one never addressed
    std::vector<Local_id> choices {};      // D75: the arms' addresses, one of which it holds
};

// Each loan `borrow` may hold: its choices' loans, or `borrow` itself.
template <class Fn>
void for_each_choice( const std::vector<Borrow>& borrows, const Borrow& borrow, Fn&& fn )
{
    if( borrow.choices.empty() )
    {
        fn( borrow );
        return;
    }

    for( const Local_id choice : borrow.choices )
    {
        for_each_choice( borrows, borrows[choice.v], fn );
    }
}

// Whether any loan `borrow` may hold meets `pred`.
template <class Pred>
bool any_choice( const std::vector<Borrow>& borrows, const Borrow& borrow, Pred&& pred )
{
    bool found = false;
    for_each_choice( borrows, borrow, [&]( const Borrow& choice ) { found = found || pred( choice ); } );
    return found;
}

// One per local, indexed by Local_id.
std::vector<Borrow> borrows_of( const Function& func, const std::vector<bool>& addressing, const Literal_pool& literals );

// One place is a prefix of the other: `p` overlaps `p.b`, and `p.a` does not, nor `p[0]` and `p[1]`.
bool overlaps( const Function& func, const std::vector<Borrow>& borrows, const Place& a, const Place& b );

// Two overlapping places that are the same element only if two indices are equal. Two copies of one
// unaddressed local are one index, since nothing in one call's arguments can assign it.
bool may_differ( const Function& func, const std::vector<Borrow>& borrows, const Place& a, const Place& b );

// D54: a call's returned address borrows each of its borrowed arguments, the receiver included.
// Ends because `through` names an earlier statement, assigned once.
bool reaches( const Function& func, const std::vector<Borrow>& borrows, const Borrow& element, const Place& whole );

// The most calls between a returned address and `whole`, or 0 when it does not reach it.
u32 reaches_depth( const Function& func, const std::vector<Borrow>& borrows, const Borrow& element, const Place& whole );

// `loan` borrows from `place`, by its own place or through its call.
bool borrows_from( const Function& func, const std::vector<Borrow>& borrows, const Borrow& loan, const Place& place );

} // namespace keel
