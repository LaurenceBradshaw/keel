// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "egraph/field.h"
#include <algorithm>
#include <cassert>
#include <numbers>
#include <numeric>

namespace keel
{
namespace
{
// A step of a product's or an inverse's inner loop takes about an eighth of a search step's time.
constexpr u64 k_field_steps_per_unit = 8;

// Phi_n, low coefficient first: x^n - 1 over the Phi_d of its proper divisors.
std::vector<i64> cyclotomic( u32 n )
{
    std::vector<std::vector<i64>> found( n + 1 );
    for( u32 d = 1; d <= n; ++d )
    {
        if( n % d != 0 )
        {
            continue;
        }

        std::vector<i64> p( d + 1, 0 );
        p[0] = -1;
        p[d] = 1;
        for( u32 e = 1; e < d; ++e )
        {
            if( d % e != 0 || found[e].empty() )
            {
                continue;
            }

            const std::vector<i64>& q = found[e];
            std::vector<i64>        quotient( p.size() - q.size() + 1, 0 );
            for( size_t i = p.size(); i-- > q.size() - 1; ) // q is monic
            {
                const i64 c                = p[i];
                quotient[i + 1 - q.size()] = c;
                for( size_t k = 0; k < q.size(); ++k )
                {
                    p[i + 1 - q.size() + k] -= c * q[k];
                }
            }
            p = std::move( quotient );
        }
        found[d] = std::move( p );
    }
    return found[n];
}
} // namespace

Field::Field( Ring_kind kind, u32 n, u32 width )
    : kind_( kind ),
      n_( n ),
      width_( kind == Ring_kind::Integer ? width : 64 ),
      phi_( cyclotomic( n ) )
{
    assert( n >= 1 && width_ >= 1 && width_ <= 64 );
    assert( kind != Ring_kind::Integer || n == 1 || n == 2 || n == 4 );
    degree_ = static_cast<u32>( phi_.size() - 1 );

    std::vector<i64> constant( degree_, 0 );
    zero_       = intern( constant, 1 );
    constant[0] = 1;
    one_        = intern( constant, 1 );
    constant[0] = plus( 0, -1 );
    minus_one_  = intern( constant, 1 );

    roots_.push_back( one_ );
    while( roots_.size() < n_ )
    {
        roots_.push_back( shift( roots_.back(), 1 ) );
    }
    for( u32 k = 0; k < n_; ++k )
    {
        mark_root( roots_[k], k, 1 );
    }
    for( u32 k = 0; k < n_; ++k )
    {
        const Entry&     entry = entries_[roots_[k].v];
        std::vector<i64> negated( entry.numerator.size() );
        std::ranges::transform( entry.numerator, negated.begin(), [&]( i64 c ) { return times( c, -1 ); } );
        negated_roots_.push_back( intern( std::move( negated ), entry.denominator ) );
        mark_root( negated_roots_.back(), k, -1 );
    }

    units_ = { one_, minus_one_ };
    if( n_ % 4 == 0 && kind_ != Ring_kind::Real )
    {
        units_.push_back( roots_[n_ / 4] );
        units_.push_back( roots_[3 * n_ / 4] );
    }
}

size_t Field::Key_hash::operator()( const std::vector<i64>& key ) const
{
    u64 h = 0xcbf29ce484222325;
    for( const i64 c : key )
    {
        h = ( h ^ static_cast<u64>( c ) ) * 0x100000001b3;
    }
    return static_cast<size_t>( h );
}

i64 Field::plus( i64 a, i64 b ) const
{
    if( kind_ == Ring_kind::Integer )
    {
        const u32 drop = 64 - width_;
        return static_cast<i64>( ( static_cast<u64>( a ) + static_cast<u64>( b ) ) << drop ) >> drop;
    }

    i64 r;
    if( __builtin_add_overflow( a, b, &r ) || r == INT64_MIN )
    {
        throw Overflow {};
    }
    return r;
}

i64 Field::times( i64 a, i64 b ) const
{
    if( kind_ == Ring_kind::Integer )
    {
        const u32 drop = 64 - width_;
        return static_cast<i64>( ( static_cast<u64>( a ) * static_cast<u64>( b ) ) << drop ) >> drop;
    }

    i64 r;
    if( __builtin_mul_overflow( a, b, &r ) || r == INT64_MIN )
    {
        throw Overflow {};
    }
    return r;
}

void Field::reduce( std::vector<i64>& p ) const
{
    for( size_t d = p.size(); d-- > degree_; )
    {
        const i64 c = p[d];
        if( c == 0 )
        {
            continue;
        }
        for( u32 k = 0; k < degree_; ++k )
        {
            p[d - degree_ + k] = plus( p[d - degree_ + k], times( c, -phi_[k] ) );
        }
    }
    p.resize( degree_ );
}

Element Field::intern( std::vector<i64> numerator, i64 denominator )
{
    if( kind_ != Ring_kind::Integer )
    {
        if( denominator < 0 )
        {
            denominator = -denominator;
            for( i64& c : numerator )
            {
                c = -c;
            }
        }
        i64 g = denominator;
        for( const i64 c : numerator )
        {
            g = std::gcd( g, c );
        }
        for( i64& c : numerator )
        {
            c /= g;
        }
        denominator /= g;
    }

    std::vector<i64> key = numerator;
    key.push_back( denominator );
    const auto [at, added] = ids_.try_emplace( std::move( key ), static_cast<u32>( entries_.size() ) );
    if( added )
    {
        std::complex<f64> value = 0;
        for( u32 k = 0; k < degree_; ++k )
        {
            if( numerator[k] != 0 )
            {
                value += static_cast<f64>( numerator[k] ) / static_cast<f64>( denominator ) *
                         std::polar( 1.0, -2 * std::numbers::pi * k / n_ );
            }
        }
        entries_.push_back( { std::move( numerator ), denominator, value } );
    }
    return Element { at->second };
}

Element Field::shift( Element a, u32 k )
{
    const Entry&     entry = entries_[a.v];
    std::vector<i64> p( degree_ + k, 0 );
    std::ranges::copy( entry.numerator, p.begin() + k );
    reduce( p );
    return intern( std::move( p ), entry.denominator );
}

Element Field::rational( i64 numerator, i64 denominator )
{
    assert( denominator != 0 );
    std::vector<i64> p( degree_, 0 );
    p[0] = plus( numerator, 0 );
    if( kind_ != Ring_kind::Integer )
    {
        return intern( std::move( p ), denominator );
    }

    const Element whole = intern( std::move( p ), 1 );
    return denominator == 1 ? whole : div( whole, rational( denominator ) );
}

Element Field::polynomial( std::vector<i64> coefficients )
{
    for( i64& c : coefficients )
    {
        c = plus( c, 0 );
    }
    reduce( coefficients );
    return intern( std::move( coefficients ), 1 );
}

std::pair<i64, i64> Field::coefficient( Element a, u32 k ) const
{
    const Entry& entry = entries_[a.v];
    return { entry.numerator[k], entry.denominator };
}

Element Field::add( Element a, Element b )
{
    if( a > b )
    {
        std::swap( a, b );
    }
    if( a == zero_ )
    {
        return b;
    }

    const u64 key = u64 { a.v } << 32 | b.v;
    if( const u32 found = sums_.find( key ); found != k_unknown )
    {
        return Element { found };
    }

    const Entry& x  = entries_[a.v];
    const Entry& y  = entries_[b.v];
    const i64    g  = std::gcd( x.denominator, y.denominator );
    const i64    l  = times( x.denominator / g, y.denominator );
    const i64    sx = l / x.denominator;
    const i64    sy = l / y.denominator;

    std::vector<i64> p( degree_ );
    for( u32 k = 0; k < degree_; ++k )
    {
        p[k] = plus( times( x.numerator[k], sx ), times( y.numerator[k], sy ) );
    }
    const Element r = intern( std::move( p ), l );
    sums_.put( key, r.v );
    return r;
}

Element Field::neg( Element a )
{
    return mul( minus_one_, a );
}

Element Field::mul( Element a, Element b )
{
    if( a > b )
    {
        std::swap( a, b );
    }
    if( a == zero_ )
    {
        return zero_;
    }
    if( a == one_ )
    {
        return b;
    }

    const Entry& ea = entries_[a.v];
    const Entry& eb = entries_[b.v];
    const bool   ra = ea.root_k != k_unknown;
    const bool   rb = eb.root_k != k_unknown;
    if( ra && rb )
    {
        const u32 k = ( ea.root_k + eb.root_k ) % n_;
        return ea.root_sign * eb.root_sign > 0 ? roots_[k] : negated_roots_[k];
    }

    const u64 key = u64 { a.v } << 32 | b.v;
    if( const u32 found = products_.find( key ); found != k_unknown )
    {
        return Element { found };
    }
    spent_ += 1 + u64 { degree_ } * degree_ / k_field_steps_per_unit;

    Element r;
    if( ra || rb )
    {
        const u32     k     = ra ? ea.root_k : eb.root_k;
        const i32     sign  = ra ? ea.root_sign : eb.root_sign;
        const Element other = ra ? b : a;
        r                   = shift( other, k );
        if( sign < 0 )
        {
            const Entry&     entry = entries_[r.v];
            std::vector<i64> negated( entry.numerator.size() );
            std::ranges::transform( entry.numerator, negated.begin(), [&]( i64 c ) { return times( c, -1 ); } );
            r = intern( std::move( negated ), entry.denominator );
        }
    }
    else
    {
        const Entry&     x = entries_[a.v];
        const Entry&     y = entries_[b.v];
        std::vector<i64> p( 2 * degree_ - 1, 0 );
        for( u32 i = 0; i < degree_; ++i )
        {
            if( x.numerator[i] == 0 )
            {
                continue;
            }
            for( u32 j = 0; j < degree_; ++j )
            {
                p[i + j] = plus( p[i + j], times( x.numerator[i], y.numerator[j] ) );
            }
        }
        reduce( p );
        r = intern( std::move( p ), times( x.denominator, y.denominator ) );
    }
    products_.put( key, r.v );
    return r;
}

bool Field::invertible( Element a )
{
    if( kind_ != Ring_kind::Integer )
    {
        return a != zero_;
    }
    // For n up to 4, a times its conjugate is an integer.
    return ( entries_[mul( a, conjugate( a ) ).v].numerator[0] & 1 ) != 0;
}

Element Field::inv( Element a )
{
    assert( invertible( a ) );
    if( const Entry& e = entries_[a.v]; e.root_k != k_unknown )
    {
        const u32 k = ( n_ - e.root_k ) % n_;
        return e.root_sign > 0 ? roots_[k] : negated_roots_[k];
    }
    if( const u32 found = entries_[a.v].inverse; found != k_unknown )
    {
        return Element { found };
    }
    spent_ += 1 + u64 { degree_ } * degree_ * degree_ / k_field_steps_per_unit;

    Element r;
    if( kind_ == Ring_kind::Integer )
    {
        const Element conjugated = conjugate( a );
        const u64     norm       = static_cast<u64>( entries_[mul( a, conjugated ).v].numerator[0] );
        u64           x          = norm; // Newton: each step doubles the bits of 1/norm that are right
        for( u32 step = 0; step < 5; ++step )
        {
            x *= 2 - norm * x;
        }
        std::vector<i64> p( degree_, 0 );
        p[0] = plus( static_cast<i64>( x ), 0 );
        r    = mul( conjugated, intern( std::move( p ), 1 ) );
    }
    else
    {
        // a = A / d, so A x = d. Column j of the system is A W^j; rows stay integer, cut by their gcd.
        const Entry&                  entry = entries_[a.v];
        std::vector<std::vector<i64>> m( degree_, std::vector<i64>( degree_ + 1, 0 ) );
        std::vector<i64>              column = entry.numerator;
        for( u32 j = 0; j < degree_; ++j )
        {
            for( u32 i = 0; i < degree_; ++i )
            {
                m[i][j] = column[i];
            }
            column.insert( column.begin(), 0 );
            reduce( column );
        }
        m[0][degree_] = entry.denominator;

        const auto cut = []( std::vector<i64>& row )
        {
            i64 g = 0;
            for( const i64 c : row )
            {
                g = std::gcd( g, c );
            }
            if( g > 1 )
            {
                for( i64& c : row )
                {
                    c /= g;
                }
            }
        };
        std::vector<__int128> wide( degree_ + 1 );
        for( u32 c = 0; c < degree_; ++c )
        {
            u32 pivot = c;
            while( m[pivot][c] == 0 )
            {
                ++pivot;
            }
            std::swap( m[pivot], m[c] );
            cut( m[c] );
            for( u32 row = 0; row < degree_; ++row )
            {
                if( row == c || m[row][c] == 0 )
                {
                    continue;
                }
                // Each product fits 126 bits; the row is cut by its gcd before it narrows again.
                const i64 g     = std::gcd( m[c][c], m[row][c] );
                const i64 scale = m[c][c] / g;
                const i64 take  = m[row][c] / g;
                __int128  h     = 0;
                for( u32 k = 0; k <= degree_; ++k )
                {
                    wide[k] = __int128 { m[row][k] } * scale - __int128 { m[c][k] } * take;
                    for( __int128 x = wide[k] < 0 ? -wide[k] : wide[k]; x != 0; )
                    {
                        const __int128 t = h % x;
                        h                = x;
                        x                = t;
                    }
                }
                for( u32 k = 0; k <= degree_; ++k )
                {
                    const __int128 x = h > 1 ? wide[k] / h : wide[k];
                    if( x > INT64_MAX || x < -INT64_MAX )
                    {
                        throw Overflow {};
                    }
                    m[row][k] = static_cast<i64>( x );
                }
            }
        }

        i64 l = 1;
        for( u32 i = 0; i < degree_; ++i )
        {
            const i64 d = m[i][i] < 0 ? -m[i][i] : m[i][i];
            l           = times( l / std::gcd( l, d ), d );
        }
        std::vector<i64> p( degree_ );
        for( u32 i = 0; i < degree_; ++i )
        {
            p[i] = times( m[i][degree_], l / m[i][i] );
        }
        r = intern( std::move( p ), l );
    }
    entries_[a.v].inverse = r.v;
    return r;
}

Element Field::div( Element a, Element b )
{
    return mul( a, inv( b ) );
}

Element Field::conjugate( Element a )
{
    const Entry&     entry = entries_[a.v];
    std::vector<i64> p( std::max( n_, degree_ ), 0 );
    for( u32 k = 0; k < degree_; ++k )
    {
        p[( n_ - k ) % n_] = plus( p[( n_ - k ) % n_], entry.numerator[k] );
    }
    reduce( p );
    return intern( std::move( p ), entry.denominator );
}

bool Field::is_unit( Element a ) const
{
    return std::ranges::find( units_, a ) != units_.end();
}

std::pair<Element, Element> Field::orbit( Element a )
{
    if( const Entry& e = entries_[a.v]; e.orbit != k_unknown )
    {
        return { Element { e.orbit }, Element { e.orbit_unit } };
    }

    std::pair<Element, Element> best { mul( units_[0], a ), units_[0] };
    for( const Element u : units_ )
    {
        best = std::min( best, std::pair { mul( u, a ), u } );
    }
    entries_[a.v].orbit      = best.first.v;
    entries_[a.v].orbit_unit = best.second.v;
    return best;
}

void Field::mark_root( Element a, u32 k, i32 sign )
{
    if( entries_[a.v].root_k == k_unknown )
    {
        entries_[a.v].root_k    = k;
        entries_[a.v].root_sign = sign;
    }
}

size_t Field::Pair_memo::index( u64 key ) const
{
    return static_cast<size_t>( ( key * 0x9E3779B97F4A7C15ull ) >> 32 ) & ( slots_.size() - 1 );
}

u32 Field::Pair_memo::find( u64 key ) const
{
    for( size_t i = index( key );; i = ( i + 1 ) & ( slots_.size() - 1 ) )
    {
        if( slots_[i].key == key )
        {
            return slots_[i].value;
        }
        if( slots_[i].key == ~u64 { 0 } )
        {
            return k_unknown;
        }
    }
}

// Kept at most half full, so a probe ends.
void Field::Pair_memo::put( u64 key, u32 value )
{
    if( 2 * ( used_ + 1 ) > slots_.size() )
    {
        std::vector<Slot> old = std::exchange( slots_, std::vector<Slot>( 2 * slots_.size() ) );
        used_                 = 0;
        for( const Slot& s : old )
        {
            if( s.key != ~u64 { 0 } )
            {
                put( s.key, s.value );
            }
        }
    }
    size_t i = index( key );
    while( slots_[i].key != ~u64 { 0 } && slots_[i].key != key )
    {
        i = ( i + 1 ) & ( slots_.size() - 1 );
    }
    used_ += slots_[i].key == ~u64 { 0 };
    slots_[i] = { key, value };
}
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{
namespace
{
bool near( std::complex<f64> a, std::complex<f64> b )
{
    return std::abs( a - b ) < 1e-9 * ( 1 + std::abs( b ) );
}

// 1 + 2W - W^3/3, nothing special in any field.
Element sample( Field& field )
{
    return field.add(
        field.add( field.one(), field.mul( field.rational( 2 ), field.root( 1 ) ) ),
        field.mul( field.rational( -1, 3 ), field.root( 3 ) )
    );
}
} // namespace

TEST_CASE( "field_has_the_degree_of_the_totient", "[egraph][field]" )
{
    for( const auto [n, phi] :
         { std::pair { 1u, 1u }, { 2u, 1u }, { 4u, 2u }, { 12u, 4u }, { 13u, 12u }, { 61u, 60u }, { 64u, 32u } } )
    {
        CHECK( Field( Ring_kind::Complex, n ).degree() == phi );
    }
}

TEST_CASE( "field_roots_multiply_by_adding_powers", "[egraph][field]" )
{
    for( const u32 n : { 3u, 8u, 12u, 15u } )
    {
        Field field( Ring_kind::Complex, n );
        for( u32 k = 0; k < n; ++k )
        {
            CHECK( near( field.value( field.root( k ) ), std::polar( 1.0, -2 * std::numbers::pi * k / n ) ) );
            CHECK( field.mul( field.root( k ), field.root( 2 ) ) == field.root( k + 2 ) );
        }
        CHECK( field.add( field.one(), field.minus_one() ) == field.zero() );
    }
    Field even( Ring_kind::Complex, 8 );
    CHECK( even.root( 4 ) == even.minus_one() );
}

TEST_CASE( "field_interns_one_id_per_value", "[egraph][field]" )
{
    Field         field( Ring_kind::Complex, 8 );
    const Element half = field.rational( 1, 2 );
    // cos(pi/4) = (W + W^-1)/2, made two ways.
    const Element a = field.mul( half, field.add( field.root( 1 ), field.root( 7 ) ) );
    const Element b = field.add( field.mul( field.root( 7 ), half ), field.mul( half, field.root( 1 ) ) );
    CHECK( a == b );
    CHECK( near( field.value( a ), { std::sqrt( 0.5 ), 0 } ) );
    CHECK( field.rational( 2, 4 ) == field.rational( -3, -6 ) );
}

TEST_CASE( "field_inverts_any_nonzero", "[egraph][field]" )
{
    for( const u32 n : { 5u, 8u, 12u, 13u, 17u } )
    {
        Field         field( Ring_kind::Complex, n );
        const Element a = sample( field );
        const Element x = field.inv( a );
        CHECK( field.mul( a, x ) == field.one() );
        CHECK( near( field.value( x ), 1.0 / field.value( a ) ) );
        CHECK( field.inv( field.root( 1 ) ) == field.root( n - 1 ) );
        CHECK( field.div( a, a ) == field.one() );
        CHECK( field.spent() > 0 );
    }
}

TEST_CASE( "field_conjugates", "[egraph][field]" )
{
    Field         field( Ring_kind::Complex, 12 );
    const Element a = sample( field );
    CHECK( near( field.value( field.conjugate( a ) ), std::conj( field.value( a ) ) ) );
}

TEST_CASE( "field_units_are_real_only_for_a_real_map", "[egraph][field]" )
{
    Field complex( Ring_kind::Complex, 8 );
    CHECK( complex.units().size() == 4 );
    CHECK( complex.is_unit( complex.root( 2 ) ) );
    Field real( Ring_kind::Real, 8 );
    CHECK( real.units().size() == 2 );
    CHECK( !real.is_unit( real.root( 2 ) ) );
    CHECK( Field( Ring_kind::Complex, 6 ).units().size() == 2 );
}

TEST_CASE( "field_orbit_is_the_least_unit_multiple", "[egraph][field]" )
{
    Field         field( Ring_kind::Complex, 8 );
    const Element a        = sample( field );
    const auto [rep, unit] = field.orbit( a );
    CHECK( rep == field.mul( unit, a ) );
    for( const Element u : field.units() )
    {
        CHECK( rep <= field.mul( u, a ) );
        CHECK( field.orbit( field.mul( u, a ) ).first == rep );
    }
}

TEST_CASE( "field_integers_wrap_and_invert_only_odd", "[egraph][field]" )
{
    Field bytes( Ring_kind::Integer, 2, 8 );
    CHECK( bytes.add( bytes.rational( 128 ), bytes.rational( 128 ) ) == bytes.zero() );
    CHECK( bytes.mul( bytes.rational( 16 ), bytes.rational( 16 ) ) == bytes.zero() );
    CHECK( bytes.rational( 255 ) == bytes.minus_one() );
    CHECK( bytes.root( 1 ) == bytes.minus_one() );
    CHECK( bytes.mul( bytes.rational( 3 ), bytes.inv( bytes.rational( 3 ) ) ) == bytes.one() );
    CHECK( !bytes.invertible( bytes.rational( 2 ) ) );
    CHECK( bytes.rational( 1, 3 ) == bytes.inv( bytes.rational( 3 ) ) );

    Field words( Ring_kind::Integer, 1, 64 );
    CHECK( words.mul( words.rational( 7 ), words.inv( words.rational( 7 ) ) ) == words.one() );
    CHECK( words.mul( words.rational( INT64_MAX ), words.rational( 2 ) ) == words.rational( -2 ) );

    // Gaussian: 1 + 2i has norm 5, 1 + i has norm 2.
    Field         gaussian( Ring_kind::Integer, 4, 32 );
    const Element odd = gaussian.add( gaussian.one(), gaussian.mul( gaussian.rational( 2 ), gaussian.root( 1 ) ) );
    CHECK( gaussian.mul( odd, gaussian.inv( odd ) ) == gaussian.one() );
    CHECK( !gaussian.invertible( gaussian.add( gaussian.one(), gaussian.root( 1 ) ) ) );
    CHECK( gaussian.units().size() == 4 );
}

TEST_CASE( "field_overflow_throws", "[egraph][field]" )
{
    Field         field( Ring_kind::Complex, 4 );
    const Element big = field.rational( INT64_MAX / 2 + 1 );
    CHECK_THROWS_AS( field.add( big, big ), Field::Overflow );
    CHECK_THROWS_AS( field.mul( big, field.rational( 3 ) ), Field::Overflow );
    CHECK_THROWS_AS( field.add( field.rational( 1, INT64_MAX ), field.rational( 1, INT64_MAX - 1 ) ), Field::Overflow );
}
} // namespace keel
#endif // ENABLE_UNIT_TESTS
