// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <complex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/types.h"

namespace keel
{
// Complex: Q(W_n), W = exp(-2pii/n). Real: the same numbers for a map whose data is real, so only
// +-1 are free units. Integer: Z/2^w[W_n], n of 1, 2 or 4, where only an odd norm inverts.
enum class Ring_kind : u8
{
    Complex,
    Real,
    Integer,
};

struct Element
{
    u32 v = 0;

    auto operator<=>( const Element& other ) const = default;
};

// A linear map's coefficients: integer vectors over one denominator in the power basis mod Phi_n,
// interned so equal elements share an id, in the order they were first made.
class Field
{
public:
    // A coefficient left i64; the candidate being tried fails.
    struct Overflow
    {
    };

    Field( Ring_kind kind, u32 n, u32 width = 64 );

    Ring_kind kind() const
    {
        return kind_;
    }

    u32 n() const
    {
        return n_;
    }

    // An integer ring's bits; 64 otherwise.
    u32 width() const
    {
        return width_;
    }

    // totient(n), the length of an element's vector.
    u32 degree() const
    {
        return degree_;
    }

    Element zero() const
    {
        return zero_;
    }

    Element one() const
    {
        return one_;
    }

    Element minus_one() const
    {
        return minus_one_;
    }

    Element root( u32 k ) const
    {
        return roots_[k % n_];
    }

    Element rational( i64 numerator, i64 denominator = 1 );
    // The polynomial with these coefficients, low first, reduced mod Phi_n.
    Element polynomial( std::vector<i64> coefficients );
    // Coefficient k of `a` in the power basis, as numerator and denominator.
    std::pair<i64, i64> coefficient( Element a, u32 k ) const;
    Element             add( Element a, Element b );
    Element             neg( Element a );
    Element             mul( Element a, Element b );
    bool                invertible( Element a );
    Element             inv( Element a );
    Element             div( Element a, Element b );
    Element             conjugate( Element a );

    // Phi_n, low coefficient first, monic.
    const std::vector<i64>& phi() const
    {
        return phi_;
    }

    // +-1, and +-W^(n/4) unless real.
    const std::vector<Element>& units() const
    {
        return units_;
    }

    bool is_unit( Element a ) const;

    // The least `u * a` by id over the units, and its `u`.
    std::pair<Element, Element> orbit( Element a );

    std::complex<f64> value( Element a ) const
    {
        return entries_[a.v].value;
    }

    // Work done, in the units the DP's fuel counts.
    u64 spent() const
    {
        return spent_;
    }

private:
    static constexpr u32 k_unknown = ~u32 { 0 };

    struct Entry
    {
        std::vector<i64>  numerator;
        i64               denominator;
        std::complex<f64> value;
        u32               root_k     = k_unknown; // +-W^k as (k, sign)
        i32               root_sign  = 0;
        u32               inverse    = k_unknown;
        u32               orbit      = k_unknown;
        u32               orbit_unit = k_unknown;
    };

    // Open addressing on a key that is never ~0: sums and products of two ids.
    class Pair_memo
    {
    public:
        u32  find( u64 key ) const;
        void put( u64 key, u32 value );

    private:
        struct Slot
        {
            u64 key = ~u64 { 0 };
            u32 value;
        };
        size_t            index( u64 key ) const;
        std::vector<Slot> slots_ = std::vector<Slot>( 1024 );
        size_t            used_  = 0;
    };

    struct Key_hash
    {
        size_t operator()( const std::vector<i64>& key ) const;
    };

    Element intern( std::vector<i64> numerator, i64 denominator );
    void    reduce( std::vector<i64>& p ) const;
    i64     plus( i64 a, i64 b ) const;
    i64     times( i64 a, i64 b ) const;
    Element shift( Element a, u32 k );
    void    mark_root( Element a, u32 k, i32 sign ); // the first k and sign set win

    Ring_kind        kind_;
    u32              n_;
    u32              width_;
    u32              degree_;
    std::vector<i64> phi_; // Phi_n, low coefficient first, monic

    std::vector<Entry>                                  entries_;
    std::unordered_map<std::vector<i64>, u32, Key_hash> ids_; // numerator, then denominator
    Pair_memo                                           products_;
    Pair_memo                                           sums_;

    Element              zero_, one_, minus_one_;
    std::vector<Element> roots_;
    std::vector<Element> negated_roots_;
    std::vector<Element> units_;
    u64                  spent_ = 0;
};
} // namespace keel
