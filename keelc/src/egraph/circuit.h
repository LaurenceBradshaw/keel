// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <complex>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "common/types.h"
#include "egraph/field.h"

namespace keel
{
enum class Op_kind : u8
{
    Input,
    Add,
    Scale,
};

// Input: `a` is the input. Add: `a <= b` are nodes. Scale: node `a` times constant `b`.
struct Op
{
    Op_kind kind;
    u32     a;
    u32     b;
};

struct Cost
{
    u32 adds = 0;
    u32 muls = 0;

    bool operator==( const Cost& ) const = default;
};

// A row of a linear map, the key of the output that computes it.
using Row = std::vector<Element>;

// A hash-consed straight-line circuit over inputs, adds and scalings. Constants are snapped complex
// floats, since sub-circuits from other fields are pasted in; an integer ring's stay exact.
class Circuit
{
public:
    explicit Circuit( bool real );
    explicit Circuit( Field& integers );

    u32 input( u32 j );
    u32 add( u32 a, u32 b );
    u32 scale( u32 a, std::complex<f64> c );
    u32 scale( u32 a, Element c );

    // Builds `sub` here with its inputs bound to `inputs`; maps each of sub's nodes to one here.
    std::vector<u32> paste( const Circuit& sub, const std::vector<u32>& inputs );

    // The live nodes, with a(bx) folded to (ab)x where bx has no other user.
    Circuit pruned() const;

    Cost cost() const;

    // Every node's value for the given inputs.
    std::vector<std::complex<f64>> evaluate( const std::vector<std::complex<f64>>& inputs ) const;

    std::string text() const;

    // Every op and constant verbatim, and each of `rows`' outputs by its index; load() reads it back
    // as the same circuit, or nothing for text it did not write.
    std::string                   saved( const std::vector<Row>& rows ) const;
    static std::optional<Circuit> load( std::string_view text, Field& field, const std::vector<Row>& rows );

    const std::vector<Op>& ops() const
    {
        return ops_;
    }

    std::complex<f64> constant( u32 c ) const
    {
        return constants_[c].value;
    }

    // An integer ring's constant, exact.
    Element exact( u32 c ) const
    {
        return constants_[c].exact;
    }

    // The nodes some output reaches.
    std::vector<bool> live() const;

    std::map<Row, u32> outputs;

private:
    struct Constant
    {
        std::complex<f64> value;
        Element           exact; // in `integers_` only
    };

    u32      put( Op op );
    u32      scale_by( u32 a, Constant c );
    u32      scale_node( u32 a, Constant c );
    Constant make( std::complex<f64> c ) const;
    Constant make( Element c ) const;
    Constant times( Constant x, Constant y ) const;
    bool     is_unit( Constant c ) const;

    bool   real_;
    Field* integers_ = nullptr;

    std::vector<Op>                    ops_;
    std::unordered_map<u64, u32>       memo_;
    std::vector<Constant>              constants_;
    std::map<std::pair<f64, f64>, u32> constant_ids_;
};
} // namespace keel
