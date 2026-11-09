// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <unordered_map>
#include <vector>
#include "codegen/description.h"
#include "sema/type.h"

namespace keel
{

// What a layout depends on in the machine. One row per target.
struct Target
{
    u64 pointer_size     = 8;
    u64 pointer_align    = 8;
    u64 max_scalar_align = 8; // an 8-byte integer or float aligns to at most this
};

inline constexpr Target x86_64_linux {};

// Every sized type's size, alignment and field offsets, as both backends lay it out. A struct's
// fields in order, a `void` one taking nothing; an instance holding nothing is one byte (D51); a
// payload enum and an error union are their tag, then a union of each variant's fields or of the
// members; an interface or run reference is two words.
class Layout
{
public:
    Layout( const Description& program, const Type_table& table, Target target = x86_64_linux );

    u64 size( Type_id type ) const;
    u64 align( Type_id type ) const;

    // From the aggregate's start: a struct's field, a closure's capture or a payload field.
    u64 offset( Type_id aggregate, Node_id field ) const;

    // Where a payload enum's or an error union's union starts, after its tag at 0.
    u64 payload( Type_id type ) const;

private:
    struct Entry
    {
        u64                  size    = 0;
        u64                  align   = 1;
        u64                  payload = 0;
        std::vector<Node_id> fields {};
        std::vector<u64>     offsets {}; // parallel to fields
    };

    const Entry& entry( Type_id type ) const;
    Entry        compute( Type_id type ) const;
    Entry        scalar( u64 bytes ) const;

    const Description& program_;
    const Type_table&  table_;
    Target             target_;

    mutable std::unordered_map<u32, Entry> entries_;
};

} // namespace keel
