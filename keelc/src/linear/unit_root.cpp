// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linear/unit_root.h"
#include <cassert>

namespace keel
{
namespace
{
// hi + lo, |lo| at most half an ulp of hi: about 106 bits, from IEEE adds and multiplies alone.
struct Double
{
    f64 hi = 0;
    f64 lo = 0;
};

Double two_sum( f64 a, f64 b )
{
    const f64 s = a + b;
    const f64 v = s - a;
    return { s, ( a - ( s - v ) ) + ( b - v ) };
}

// Dekker's split, as no FMA is assumed.
Double two_product( f64 a, f64 b )
{
    const auto split = []( f64 x )
    {
        const f64 t = 134217729.0 * x; // 2^27 + 1
        const f64 h = t - ( t - x );
        return Double { h, x - h };
    };
    const f64    p = a * b;
    const Double x = split( a );
    const Double y = split( b );
    return { p, ( ( x.hi * y.hi - p ) + x.hi * y.lo + x.lo * y.hi ) + x.lo * y.lo };
}

Double operator+( Double a, Double b )
{
    const Double s = two_sum( a.hi, b.hi );
    return two_sum( s.hi, s.lo + a.lo + b.lo );
}

Double operator*( Double a, Double b )
{
    const Double p = two_product( a.hi, b.hi );
    return two_sum( p.hi, p.lo + a.hi * b.lo + a.lo * b.hi );
}

Double over( Double a, f64 d )
{
    const f64    q = a.hi / d;
    const Double p = two_product( q, d );
    return two_sum( q, ( a.hi - p.hi - p.lo + a.lo ) / d );
}

// cos and sin of (r / n) pi / 4, r in [0, n]: Taylor series, the angle at most pi / 4.
std::pair<f64, f64> octant( u64 r, u64 n )
{
    const Double quarter_pi { 0x1.921fb54442d18p-1, 0x1.1a62633145c07p-55 };
    const Double x  = over( Double { static_cast<f64>( r ) }, static_cast<f64>( n ) ) * quarter_pi;
    const Double x2 = x * x;
    Double       c { 1 }, s = x, term_c { 1 }, term_s = x;
    for( u32 i = 1; i <= 20; ++i ) // x^41 / 41! < 2^-160
    {
        term_c = over( term_c * x2, -static_cast<f64>( ( 2 * i - 1 ) * ( 2 * i ) ) );
        term_s = over( term_s * x2, -static_cast<f64>( ( 2 * i ) * ( 2 * i + 1 ) ) );
        c      = c + term_c;
        s      = s + term_s;
    }
    return { c.hi, s.hi };
}
} // namespace

std::complex<f64> unit_root( u64 k, u64 n )
{
    assert( n > 0 && n < ( u64 { 1 } << 53 ) );
    // 2 pi k / n = (o + r / n) pi / 4, exactly.
    const u64 eighths = k % n * 8;
    const u64 o       = eighths / n;
    const u64 r       = eighths % n;

    // cos and sin of (o % 2 + r / n) pi / 4: an odd octant is pi / 2 less its complement.
    auto [c, s] = octant( o % 2 == 0 ? r : n - r, n );
    if( o % 2 != 0 )
    {
        std::swap( c, s );
    }
    // Then o / 2 quarter turns. + 0.0: a zero part is +0, not -0.
    switch( o / 2 )
    {
    case 0:
        return { c, -s + 0.0 };
    case 1:
        return { -s + 0.0, -c };
    case 2:
        return { -c, s };
    default:
        return { s, c };
    }
}
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

namespace keel
{
TEST_CASE( "unit_root_gives_exact_values_where_they_are_doubles", "[linear]" )
{
    CHECK( unit_root( 0, 8 ) == std::complex<f64>( 1, 0 ) );
    CHECK( unit_root( 2, 8 ) == std::complex<f64>( 0, -1 ) );
    CHECK( unit_root( 4, 8 ) == std::complex<f64>( -1, 0 ) );
    CHECK( unit_root( 6, 8 ) == std::complex<f64>( 0, 1 ) );
    CHECK( !std::signbit( unit_root( 2, 8 ).real() ) );
    CHECK( !std::signbit( unit_root( 4, 8 ).imag() ) );
    CHECK( unit_root( 9, 8 ) == unit_root( 1, 8 ) );
    CHECK( unit_root( 1, 12 ).imag() == -0.5 );
    CHECK( unit_root( 2, 12 ).real() == 0.5 );
}

TEST_CASE( "unit_root_rounds_the_true_value", "[linear]" )
{
    // sqrt(1/2) correctly rounded; cos of pi / 4 rounded to a double is an ulp below.
    const f64 half_root_2 = 0x1.6a09e667f3bcdp-1;
    CHECK( unit_root( 1, 8 ) == std::complex<f64>( half_root_2, -half_root_2 ) );
    CHECK( unit_root( 3, 8 ) == std::complex<f64>( -half_root_2, -half_root_2 ) );
    CHECK( unit_root( 1, 12 ).real() == std::sqrt( 3.0 ) / 2 );
}

// libm's angle is rounded first, by up to 4.4e-16 near 2 pi, and the UCRT's cos and sin add more.
TEST_CASE( "unit_root_is_near_libm", "[linear]" )
{
    for( u64 n : { 3u, 5u, 7u, 16u, 96u, 1000u } )
    {
        for( u64 k = 0; k < n; ++k )
        {
            const std::complex<f64> w =
                std::polar( 1.0, -2 * std::numbers::pi * static_cast<f64>( k ) / static_cast<f64>( n ) );
            CHECK( std::abs( unit_root( k, n ) - w ) <= 2e-15 );
        }
    }
}
} // namespace keel
#endif
