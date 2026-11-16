// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <array>
#include <complex>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/types.h"
#include "linear/field.h"

namespace keel
{
enum class Op_kind : u8
{
    Input,
    Add,
    Scale,
    Real,
    Conj,
    One,
};

// Input: `a` is the input. Add: `a <= b` are nodes. Scale: node `a` times constant `b`. Real: node `a`'s real part.
// Conj: node `a`'s conjugate, free as a sign is. One: the constant 1.
struct Op
{
    Op_kind kind;
    u32     a;
    u32     b;
};

// The one cost every stage ranks by: fewer adds and multiplies in all, then fewer multiplies.
struct Cost
{
    u32 adds = 0;
    u32 muls = 0;

    bool operator==( const Cost& ) const = default;

    auto operator<=>( const Cost& o ) const
    {
        return std::pair { adds + muls, muls } <=> std::pair { o.adds + o.muls, o.muls };
    }

    Cost operator+( Cost o ) const
    {
        return { adds + o.adds, muls + o.muls };
    }

    friend Cost operator*( u32 k, Cost c )
    {
        return { k * c.adds, k * c.muls };
    }
};

// A row of a linear map, the key of the output that computes it.
using Row = std::vector<Element>;

// What scaling a value by `c` costs, as Circuit::cost counts it, `real` when the value is;
// `unit` when the ring says it is free.
Cost scale_cost( std::complex<f64> c, bool real, bool unit = false );

// A hash-consed straight-line circuit over inputs, adds, scalings, real parts and conjugates. A constant node is 1
// or 1 scaled, folded and free as a literal is. Constants are snapped complex floats, since sub-circuits from other
// fields are pasted in; an integer ring's stay exact.
// A node is real when its inputs are and its constants keep it so; only live parts are counted.
class Circuit
{
public:
    // `real`: the inputs are.
    explicit Circuit( bool real );
    explicit Circuit( Field& integers );

    u32  input( u32 j );
    u32  add( u32 a, u32 b );
    u32  scale( u32 a, std::complex<f64> c );
    u32  scale( u32 a, Element c );
    u32  real_part( u32 a );
    u32  conj( u32 a );
    u32  one();
    bool real() const
    {
        return real_;
    }

    // Builds `sub` here with its inputs bound to `inputs`, its float constants conjugated when
    // `conjugate`, so computing the conjugate map; maps each of sub's nodes to one here.
    std::vector<u32> paste( const Circuit& sub, const std::vector<u32>& inputs, bool conjugate = false );

    // The transpose over the reals, built in `into`: seeds[o] stands for output node outs[o], and node j of the
    // result is input j's, real when it is. A complex map's transpose is its conjugate transpose;
    // constants drop out.
    std::vector<u32> transposed( Circuit& into, const std::vector<u32>& outs, const std::vector<u32>& seeds, u32 inputs ) const;

    // This circuit with input `j` the constant 1.
    Circuit bound( u32 j ) const;

    // The live nodes, with a(bx) folded to (ab)x where bx has no other user.
    Circuit pruned() const;

    // With `fma`, a float multiply fused into the add after it is free, as LLVM fuses at -march with FMA.
    Cost cost( bool fma = false ) const;

    // Every node's value for the given inputs.
    std::vector<std::complex<f64>> evaluate( const std::vector<std::complex<f64>>& inputs ) const;

    std::string text() const;

    // Every op and constant verbatim, and each of `rows`' outputs by its index; load() reads it back
    // as the same circuit, or nothing for text it did not write. A circuit with calls is never saved.
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

    // Which of each node's real and imaginary parts some output reaches; a real node has no imaginary part.
    std::vector<std::array<bool, 2>> live_parts() const;

    static constexpr u32 k_zero = ~u32 { 0 };

    // Each row's output node, or k_zero for a row with none.
    std::vector<u32> nodes( const std::vector<Row>& rows ) const;

    std::map<Row, u32> outputs;

private:
    struct Constant
    {
        std::complex<f64> value;
        Element           exact; // in `integers_` only
    };

    u32 put( Op op );
    // Each live node's users, an output counting as one.
    std::vector<u32> uses( const std::vector<bool>& lv ) const;
    u32              scale_by( u32 a, Constant c );
    u32              scale_node( u32 a, Constant c );
    bool             is_constant( u32 a ) const;
    Constant         value_of( u32 a ) const; // a constant node's
    Constant         make( std::complex<f64> c ) const;
    Constant         make( Element c ) const;
    Constant         times( Constant x, Constant y ) const;
    Constant         plus( Constant x, Constant y ) const;
    bool             is_unit( Constant c ) const;

    bool   real_;
    Field* integers_ = nullptr;

    std::vector<Op>                    ops_;
    std::vector<bool>                  real_ops_;      // each op's value is real
    std::vector<bool>                  imaginary_ops_; // each op's real part is 0
    std::unordered_map<u64, u32>       memo_;
    std::vector<Constant>              constants_;
    std::map<std::pair<f64, f64>, u32> constant_ids_;
};
} // namespace keel
