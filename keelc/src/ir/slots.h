// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ir/kir.h"

namespace keel
{

// What a place covers: `node` is the slot it names, and [first, last) the slots under it. Only a
// leaf carries state; a split local's own slot holds none.
struct Slots
{
    u32  node    = 0;
    u32  first   = 0;
    u32  last    = 0;
    bool exact   = true;  // false when the place goes past a tracked node: into an enum, an array, a pointer
    bool element = false; // past it by an Index: an element of an array
};

// One slot per local, then one per Function::fields entry, so a definite-assignment, drop-flag or
// occupancy analysis can index a bit vector by slot. The count is fixed at construction: a local a
// pass appends afterwards has no slot.
class Slot_map
{
public:
    explicit Slot_map( const Function& func );

    u32 count() const
    {
        return base_ + narrow_cast<u32>( func_.fields.size() );
    }

    // A leading Deref is skipped for an `out` parameter and for `constructed`, whose trees describe
    // the referent.
    Slots of( const Place& place ) const;
    Slots under( u32 slot ) const; // the slot named whole

    bool     is_leaf( u32 slot ) const;
    u32      next( u32 slot ) const; // one past the slot's subtree
    Local_id local( u32 slot ) const;
    u32      field( u32 slot ) const;  // into Function::fields; k_no_field for a local's own slot
    u32      parent( u32 slot ) const; // the enclosing field or local; k_no_field for a local
    bool     built_whole( u32 slot ) const;

private:
    const Function&  func_;
    u32              base_;
    std::vector<u32> run_first_; // per local: its fields' run in Function::fields, empty if it does not split
    std::vector<u32> run_last_;
    std::vector<u8>  through_; // an `out` parameter or `constructed`
};

} // namespace keel
