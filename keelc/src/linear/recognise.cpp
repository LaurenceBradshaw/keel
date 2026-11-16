// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linear/recognise.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>
#include <unordered_map>
#include "ir/loops.h"
#include "opt/fold_constants.h"
#include "opt/forward_reads.h"
#include "opt/reduce_inductions.h"

namespace keel
{
namespace
{
constexpr u32 k_largest_root  = 256;  // the largest M a constant or an angle is matched at
constexpr u32 k_largest_field = 1024; // the largest M a coefficient may need
constexpr u32 k_deepest_call  = 64;

// The symbolic loop reader's values: polynomials in the loop indices, and sums over input elements.
namespace sym
{
using i128 = __int128;

constexpr u32 k_symbols      = 8;
constexpr u64 k_largest_root = u64 { 1 } << 24; // the largest M an angle is matched at

[[noreturn]] void fail()
{
    throw Unrecognised { Ending::Unsupported, {} };
}

i64 narrowed( i128 x )
{
    if( x > std::numeric_limits<i64>::max() || x < std::numeric_limits<i64>::min() )
    {
        fail();
    }
    return static_cast<i64>( x );
}

// Integer polynomials in the loop symbols, no zero coefficient.
using Monomial = std::array<u8, k_symbols>;
using Poly     = std::map<Monomial, i64>;

Poly constant( i64 c )
{
    return c == 0 ? Poly {} : Poly { { Monomial {}, c } };
}

Poly symbol( u32 s )
{
    Monomial m {};
    m[s] = 1;
    return { { m, 1 } };
}

void add_to( Poly& p, const Monomial& m, i128 c )
{
    const i64 v = narrowed( i128 { p[m] } + c );
    if( v == 0 )
    {
        p.erase( m );
    }
    else
    {
        p[m] = v;
    }
}

Poly sum( Poly a, const Poly& b, i64 sign = 1 )
{
    for( const auto& [m, c] : b )
    {
        add_to( a, m, i128 { c } * sign );
    }
    return a;
}

Poly product( const Poly& a, const Poly& b )
{
    Poly p;
    for( const auto& [ma, ca] : a )
    {
        for( const auto& [mb, cb] : b )
        {
            Monomial m;
            for( u32 s = 0; s < k_symbols; ++s )
            {
                if( ma[s] + mb[s] > 8 )
                {
                    fail();
                }
                m[s] = static_cast<u8>( ma[s] + mb[s] );
            }
            add_to( p, m, i128 { ca } * cb );
        }
    }
    return p;
}

std::optional<i64> constant_of( const Poly& p )
{
    if( p.empty() )
    {
        return 0;
    }
    if( p.size() == 1 && p.begin()->first == Monomial {} )
    {
        return p.begin()->second;
    }
    return std::nullopt;
}

bool depends( const Poly& p, u32 s )
{
    return std::ranges::any_of( p, [s]( const auto& t ) { return t.first[s] != 0; } );
}

// Coefficients in [0, M).
Poly reduced( const Poly& p, u64 M )
{
    Poly r;
    for( const auto& [m, c] : p )
    {
        i128 v = i128 { c } % static_cast<i128>( M );
        v      = v < 0 ? v + M : v;
        if( v != 0 )
        {
            r[m] = static_cast<i64>( v );
        }
    }
    return r;
}

// A value known to be p, or p mod q when q is not 0, and within [lo, hi].
struct Int
{
    Poly p;
    u64  q  = 0;
    i128 lo = 0;
    i128 hi = 0;
};

Int number( i64 c )
{
    return { constant( c ), 0, c, c };
}

bool concrete( const Int& x )
{
    return x.q == 0 && constant_of( x.p ).has_value();
}

bool same( const Int& x, const Int& y )
{
    const auto reduced_in = [&]( const Int& v ) { return v.q == 0 || ( v.lo >= 0 && v.hi < static_cast<i128>( v.q ) ); };
    return x.p == y.p && x.q == y.q && reduced_in( x ) && reduced_in( y );
}

// sum over Poly e of c W_M^e: a function of the symbols. As W^(M/2) = -1, an exponent's constant is below M/2.
struct Wave
{
    u64                      M = 1;
    std::map<Poly, Rational> terms;
};

// `e` reduced mod M.
void add_term( Wave& w, Poly e, Rational c )
{
    if( const auto at = e.find( Monomial {} ); w.M % 2 == 0 && at != e.end() && static_cast<u64>( at->second ) >= w.M / 2 )
    {
        c = -c;
        at->second -= static_cast<i64>( w.M / 2 );
        if( at->second == 0 )
        {
            e.erase( at );
        }
    }
    Rational& t = w.terms[e];
    t           = t + c;
    if( t == 0 )
    {
        w.terms.erase( e );
    }
}

Wave wave( Rational c )
{
    Wave w;
    add_term( w, {}, c );
    return w;
}

// i = -W_4.
Wave imaginary()
{
    Wave w { 4, {} };
    add_term( w, constant( 1 ), -1 );
    return w;
}

u64 common( u64 a, u64 b )
{
    const u64 m = std::lcm( a, b );
    if( m > k_largest_root )
    {
        fail();
    }
    return m;
}

Wave lifted( const Wave& w, u64 M )
{
    if( w.M == M )
    {
        return w;
    }
    Wave out { M, {} };
    for( const auto& [e, c] : w.terms )
    {
        add_term( out, reduced( product( e, constant( narrowed( M / w.M ) ) ), M ), c );
    }
    return out;
}

Wave sum( const Wave& a, const Wave& b, Rational sign = 1 )
{
    const u64 M   = common( a.M, b.M );
    Wave      out = lifted( a, M );
    for( const auto& [e, c] : lifted( b, M ).terms )
    {
        add_term( out, e, sign * c );
    }
    return out;
}

Wave product( const Wave& a, const Wave& b )
{
    const u64  M  = common( a.M, b.M );
    const Wave la = lifted( a, M );
    const Wave lb = lifted( b, M );
    Wave       out { M, {} };
    for( const auto& [ea, ca] : la.terms )
    {
        for( const auto& [eb, cb] : lb.terms )
        {
            add_term( out, reduced( sum( ea, eb ), M ), ca * cb );
        }
    }
    return out;
}

Wave scaled( const Wave& a, Rational c )
{
    Wave out { a.M, {} };
    for( const auto& [e, t] : a.terms )
    {
        add_term( out, e, c * t );
    }
    return out;
}

bool depends( const Wave& w, u32 s )
{
    return std::ranges::any_of( w.terms, [s]( const auto& t ) { return depends( t.first, s ); } );
}

// An input element, `stream` at `index`, summed over the symbols in `bound`.
struct Key
{
    u32  stream;
    Poly index;
    u8   bound;

    auto operator<=>( const Key& ) const = default;
};

// The stream of the input fixed at 1, a constant's key with no index or bound.
constexpr u32 k_one_stream = ~0u;

// A linear form over input elements; `carried` is a local's value at the turn's start, added once.
struct Form
{
    std::map<Key, Wave> terms {};
    Local_id            carried {};
};

Form sum( const Form& a, const Form& b, Rational sign )
{
    if( b.carried.is_valid() && ( sign != 1 || a.carried.is_valid() ) )
    {
        fail();
    }
    Form out = a;
    if( b.carried.is_valid() )
    {
        out.carried = b.carried;
    }
    for( const auto& [k, w] : b.terms )
    {
        const auto found = out.terms.find( k );
        Wave       n     = found == out.terms.end() ? scaled( w, sign ) : sum( found->second, w, sign );
        if( n.terms.empty() )
        {
            out.terms.erase( k );
        }
        else
        {
            out.terms[k] = std::move( n );
        }
    }
    return out;
}

Form scaled( const Form& a, const Wave& w )
{
    if( a.carried.is_valid() )
    {
        fail();
    }
    Form out;
    for( const auto& [k, t] : a.terms )
    {
        Wave n = product( t, w );
        if( !n.terms.empty() )
        {
            out.terms.emplace( k, std::move( n ) );
        }
    }
    return out;
}

// `value` written to `stream` at `index`, for each value of the symbols in `bound`.
struct Store
{
    u32  stream;
    Poly index;
    Form value;
    u8   bound = 0;
};

// An approximation p/q of x within rounding, q at most k_largest_root.
std::optional<std::pair<i64, u64>> rational( f64 x )
{
    if( x == 0 )
    {
        return std::pair<i64, u64> { 0, 1 };
    }
    if( !std::isfinite( x ) || std::abs( x ) > 1e15 )
    {
        return std::nullopt;
    }
    const f64 tolerance = 16 * std::numeric_limits<f64>::epsilon() * std::abs( x );
    i128      h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    f64       r = x;
    for( u32 n = 0; n < 64; ++n )
    {
        const f64  a  = std::floor( r );
        const i128 h2 = static_cast<i128>( a ) * h1 + h0;
        const i128 k2 = static_cast<i128>( a ) * k1 + k0;
        if( k2 > static_cast<i128>( k_largest_root ) )
        {
            return std::nullopt;
        }
        h0 = h1, h1 = h2, k0 = k1, k1 = k2;
        if( std::abs( x - static_cast<f64>( h1 ) / static_cast<f64>( k1 ) ) <= tolerance )
        {
            return std::pair<i64, u64> { narrowed( h1 ), static_cast<u64>( k1 ) };
        }
        if( r == a )
        {
            return std::nullopt;
        }
        r = 1 / ( r - a );
    }
    return std::nullopt;
}

// The simplest p/q, q at most 2^20, whose rounded f64 is x: a decimal of up to 6 places is its fraction.
std::optional<std::pair<i64, u64>> fraction( f64 x )
{
    if( !std::isfinite( x ) || std::abs( x ) > 0x1p32 )
    {
        return std::nullopt;
    }
    i64 h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    f64 r = x;
    for( u32 n = 0; n < 64; ++n )
    {
        const f64 a = std::floor( r );
        if( a * static_cast<f64>( k1 ) + static_cast<f64>( k0 ) > 0x1p20 )
        {
            return std::nullopt;
        }
        const i64 h2 = static_cast<i64>( a ) * h1 + h0;
        const i64 k2 = static_cast<i64>( a ) * k1 + k0;
        h0 = h1, h1 = h2, k0 = k1, k1 = k2;
        if( static_cast<f64>( h1 ) / static_cast<f64>( k1 ) == x )
        {
            return std::pair<i64, u64> { h1, static_cast<u64>( k1 ) };
        }
        if( r == a )
        {
            return std::nullopt;
        }
        r = 1 / ( r - a );
    }
    return std::nullopt;
}
} // namespace sym

using sym::i128;

// An element of Q(W_m), in fields_[m].
struct Exact
{
    u32     m = 1;
    Element e {};
};

enum class Kind : u8
{
    Unknown,
    Number,
    Form,
    Address,
    // Symbolic mode only, each in symbolics_.
    Int,   // a polynomial in the loop indices
    Angle, // a i + b
    Wave,
    Terms, // a sum over input elements
};

struct Value
{
    Kind  kind  = Kind::Unknown;
    u64   bits  = 0;     // Number: an integer's bits, or an f64's
    bool  exact = false; // Number: an f64 whose exact value is `value`
    Exact value {};
    u32   index = 0; // Form: into forms_; Address: into locations_; symbolic: into symbolics_
};

struct Symbolic
{
    sym::Int  i {};
    f64       a = 0;
    f64       b = 0;
    sym::Wave w {};
    sym::Form f {};
};

// A root, then steps: a field f is 4f + 1, element e is 4e, and the symbolic element polys_[poly] is 2.
struct Location
{
    u32              root = 0;
    std::vector<u64> steps;
    u32              poly = 0;

    auto operator<=>( const Location& ) const = default;
};

constexpr u64 k_symbolic_step = 2;

// The root of the input fixed at 1, past every other.
constexpr u32 k_one_root = ~0u;

// What a parameter's elements are read or written through: a root, an element, then the fields in `rest`.
struct Stream
{
    u32              root;
    std::vector<u64> rest;
    bool             read    = false;
    bool             written = false;
};

struct Root
{
    bool       input  = false;
    bool       output = false;
    Param_path path {}; // input or output only
};

// An input's coefficient: `bits` in an integer ring, else `exact`.
struct Term
{
    u32   input = 0;
    u64   bits  = 0;
    Exact exact {};
};

using Form = std::vector<Term>; // by input, no zero term

struct Cell
{
    Value value;
    Span  span; // where it was written
};

struct Frame
{
    const Function*  func;
    std::vector<u32> roots; // by local, ~0 before first use
};

class Recogniser
{
public:
    Recogniser(
        const std::vector<Function>& functions,
        const Purity&                purity,
        const Type_table&            types,
        const Literal_pool&          literals,
        u64                          budget,
        const Global_constants&      globals
    )
        : functions_( functions ),
          purity_( purity ),
          types_( types ),
          literals_( literals ),
          budget_( budget ),
          globals_( globals )
    {
    }

    std::variant<Recognised, Unrecognised> run( const Function& func )
    {
        try
        {
            Frame top = frame( func );
            set_up( top );
            execute( top );
            return collect();
        }
        catch( const Unrecognised& ended )
        {
            return ended;
        }
        catch( const Field::Overflow& )
        {
            return Unrecognised { Ending::Coefficient, span_ };
        }
    }

    // Symbolic mode: the kernel the loops compute, or nothing.
    std::optional<Read_kernel> read_sum( const Function& func )
    {
        if( func.returns_a_value )
        {
            return std::nullopt;
        }
        symbolic_  = true;
        loops_     = find_loops( func );
        addressed_ = addressed_locals_of( func );
        try
        {
            Frame top = frame( func );
            set_up( top );
            run_from( top, k_entry_block, k_no_loop );
            return collect_sum();
        }
        catch( const Unrecognised& )
        {
            return std::nullopt;
        }
        catch( const Field::Overflow& )
        {
            return std::nullopt;
        }
    }

private:
    [[noreturn]] void end( Ending why, Span span = {} ) const
    {
        throw Unrecognised { why, span.is_valid() ? span : span_ };
    }

    Field& field( u32 m )
    {
        return fields_.try_emplace( m, Ring_kind::Real, m ).first->second;
    }

    // `e` of `from` in `to`, whose n is a multiple of from's.
    static Element lift_into( Field& from, Element e, Field& to )
    {
        const u32        r = to.n() / from.n();
        std::vector<i64> p( ( from.degree() - 1 ) * r + 1, 0 );
        i64              denominator = 1;
        for( u32 k = 0; k < from.degree(); ++k )
        {
            const auto [numerator, d] = from.coefficient( e, k );
            p[k * r]                  = numerator;
            denominator               = d;
        }
        const Element whole = to.polynomial( std::move( p ) );
        return denominator == 1 ? whole : to.mul( whole, to.rational( 1, denominator ) );
    }

    Element lift( Exact a, u32 m )
    {
        if( a.m == m )
        {
            return a.e;
        }
        const u64 key = ( u64 { a.m } << 52 ) ^ ( u64 { m } << 32 ) ^ a.e.v;
        if( const auto found = lifts_.find( key ); found != lifts_.end() )
        {
            return found->second;
        }
        const Element e = lift_into( field( a.m ), a.e, field( m ) );
        lifts_.emplace( key, e );
        return e;
    }

    u32 common( u32 a, u32 b )
    {
        const u32 m = std::lcm( a, b );
        if( m > k_largest_field )
        {
            end( Ending::Coefficient );
        }
        return m;
    }

    Exact rational( i64 numerator, i64 denominator = 1 )
    {
        return { 1, field( 1 ).rational( numerator, denominator ) };
    }

    Exact sum( Exact a, Exact b )
    {
        const u32 m = common( a.m, b.m );
        return { m, field( m ).add( lift( a, m ), lift( b, m ) ) };
    }

    Exact product( Exact a, Exact b )
    {
        const u32 m = common( a.m, b.m );
        return { m, field( m ).mul( lift( a, m ), lift( b, m ) ) };
    }

    Exact negated( Exact a )
    {
        return { a.m, field( a.m ).neg( a.e ) };
    }

    bool zero( Exact a )
    {
        return a.e == field( a.m ).zero();
    }

    std::optional<Exact> inverse( Exact a )
    {
        if( zero( a ) )
        {
            return std::nullopt;
        }
        return Exact { a.m, field( a.m ).inv( a.e ) };
    }

    // a/d as a rational, when `a` is one.
    std::optional<std::pair<i64, i64>> as_rational( Exact a )
    {
        Field& f = field( a.m );
        for( u32 k = 1; k < f.degree(); ++k )
        {
            if( f.coefficient( a.e, k ).first != 0 )
            {
                return std::nullopt;
            }
        }
        return f.coefficient( a.e, 0 );
    }

    // In the least field this code finds it in: Q for a rational.
    Exact simplest( Exact a )
    {
        if( a.m == 1 )
        {
            return a;
        }
        if( const auto q = as_rational( a ) )
        {
            return rational( q->first, q->second );
        }
        return a;
    }

    // (W^k + W^-k) / 2 = cos(2 pi k / m).
    Exact cosine( u32 k, u32 m )
    {
        Field& f = field( m );
        return simplest( { m, f.mul( f.add( f.root( k ), f.root( m - k % m ) ), f.rational( 1, 2 ) ) } );
    }

    // (W^-k - W^k) W_4 / 2 = sin(2 pi k / m), as W_4 = -i.
    Exact sine( u32 k, u32 m )
    {
        const u32 n = std::lcm( m, 4u );
        Field&    f = field( n );
        const u32 j = k * ( n / m ) % n;
        return simplest(
            { n, f.mul( f.mul( f.add( f.root( n - j ), f.neg( f.root( j ) ) ), f.root( n / 4 ) ), f.rational( 1, 2 ) ) }
        );
    }

    // A short dyadic as it stands, else a fraction whose rounding it is, else within rounding of 2^s cos(2 pi k / M).
    std::optional<Exact> exact_literal( f64 x )
    {
        const u64 key = std::bit_cast<u64>( x );
        if( const auto found = literal_exacts_.find( key ); found != literal_exacts_.end() )
        {
            return found->second;
        }
        std::optional<Exact> found;
        if( std::isfinite( x ) )
        {
            for( u32 s = 0; s <= 30 && !found; ++s )
            {
                const f64 y = std::ldexp( x, static_cast<int>( s ) );
                if( std::abs( y ) < 0x1p31 && y == std::floor( y ) )
                {
                    found = rational( static_cast<i64>( y ), i64 { 1 } << s );
                }
            }
        }
        if( const auto q = found ? std::nullopt : sym::fraction( x ) )
        {
            found = rational( q->first, static_cast<i64>( q->second ) );
        }
        // 2^s cos(2 pi k / M), so a folded sqrt 2 or sqrt 3 is found too.
        for( u32 scale = 0; scale <= 3 && !found; ++scale )
        {
            const f64 c = std::ldexp( x, -static_cast<int>( scale ) );
            if( std::abs( c ) > 1 )
            {
                continue;
            }
            const f64 theta = std::acos( c );
            for( u32 m = 1; m <= k_largest_root && !found; ++m )
            {
                const i64 near = std::llround( theta * m / ( 2 * std::numbers::pi ) );
                for( i64 k = std::max<i64>( near - 1, 0 ); k <= near + 1 && !found; ++k )
                {
                    if( std::abs( std::cos( 2 * std::numbers::pi * static_cast<f64>( k ) / m ) - c ) <= 1e-15 )
                    {
                        found = simplest( product( cosine( static_cast<u32>( k ), m ), rational( i64 { 1 } << scale ) ) );
                    }
                }
            }
        }
        literal_exacts_.emplace( key, found );
        return found;
    }

    // `trig` of an angle within rounding of pi k / M.
    std::optional<Exact> exact_trig( Trig trig, f64 x )
    {
        if( !std::isfinite( x ) )
        {
            return std::nullopt;
        }
        for( u32 m = 1; m <= k_largest_root; ++m )
        {
            const f64 r = x * m / std::numbers::pi;
            const i64 k = std::llround( r );
            if( std::abs( x - std::numbers::pi * static_cast<f64>( k ) / m ) > 1e-13 * std::max( 1.0, std::abs( x ) ) )
            {
                continue;
            }
            const u32 n = 2 * m;
            const u32 j = static_cast<u32>( ( ( k % n ) + n ) % n );
            switch( trig )
            {
            case Trig::Sin:
                return sine( j, n );
            case Trig::Cos:
                return cosine( j, n );
            case Trig::Tan:
            {
                const std::optional<Exact> c = inverse( cosine( j, n ) );
                return c ? std::optional( simplest( product( sine( j, n ), *c ) ) ) : std::nullopt;
            }
            case Trig::None:
                break;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    // sqrt(q) for a rational q whose square-free part is 1, 2 or 3: sqrt 2 = W_8 + W_8^-1, sqrt 3 = W_12 + W_12^-1.
    std::optional<Exact> exact_root( Exact a )
    {
        const auto q = as_rational( a );
        if( !q || q->first < 0 || q->first > ( i64 { 1 } << 20 ) || q->second > ( i64 { 1 } << 20 ) )
        {
            return std::nullopt;
        }
        // sqrt(n / d) = sqrt(n d) / d.
        i64 rest   = q->first * q->second;
        i64 square = 1;
        i64 free   = 1;
        for( i64 p = 2; p * p <= rest; ++p )
        {
            while( rest % ( p * p ) == 0 )
            {
                rest /= p * p;
                square *= p;
            }
            if( rest % p == 0 )
            {
                rest /= p;
                free *= p;
            }
        }
        free *= rest;
        const Exact scale = rational( square, q->second );
        if( free == 1 || q->first == 0 )
        {
            return scale;
        }
        if( free != 2 && free != 3 )
        {
            return std::nullopt;
        }
        const u32 m = free == 2 ? 8 : 12;
        Field&    f = field( m );
        return product( scale, { m, f.add( f.root( 1 ), f.root( m - 1 ) ) } );
    }

    // Numbers.

    static Value number( u64 bits )
    {
        return { .kind = Kind::Number, .bits = bits };
    }

    static Value real( f64 x, std::optional<Exact> e = std::nullopt )
    {
        return {
            .kind = Kind::Number, .bits = std::bit_cast<u64>( x ), .exact = e.has_value(), .value = e.value_or( Exact {} )
        };
    }

    static f64 as_real( Value v )
    {
        return std::bit_cast<f64>( v.bits );
    }

    static std::optional<Exact> exact_of( Value v )
    {
        return v.exact ? std::optional( v.value ) : std::nullopt;
    }

    bool is_f64( Type_id type ) const
    {
        const Type& t = types_.get( type );
        return t.kind == Type_kind::Float && t.width == 64;
    }

    bool is_float( Type_id type ) const
    {
        return types_.get( type ).kind == Type_kind::Float;
    }

    bool is_aggregate( Type_id type ) const
    {
        const Type_kind kind = types_.get( type ).kind;
        return kind == Type_kind::Struct || kind == Type_kind::Array;
    }

    // Forms.

    Value form( Form f )
    {
        forms_.push_back( std::move( f ) );
        return { .kind = Kind::Form, .index = static_cast<u32>( forms_.size() - 1 ) };
    }

    u64 wrap( u64 bits ) const
    {
        return truncate( bits, width_ );
    }

    bool zero( const Term& t )
    {
        return integer_ ? t.bits == 0 : zero( t.exact );
    }

    Term term_sum( Term a, const Term& b )
    {
        a.bits  = wrap( a.bits + b.bits );
        a.exact = integer_ ? Exact {} : simplest( sum( a.exact, b.exact ) );
        return a;
    }

    Value combined( Value a, Value b, bool minus )
    {
        const Form& x = forms_[a.index];
        const Form  y = minus ? scaled( forms_[b.index], negative_one() ) : forms_[b.index];
        Form        out;
        size_t      i = 0, j = 0;
        while( i < x.size() || j < y.size() )
        {
            if( j == y.size() || ( i < x.size() && x[i].input < y[j].input ) )
            {
                out.push_back( x[i++] );
            }
            else if( i == x.size() || y[j].input < x[i].input )
            {
                out.push_back( y[j++] );
            }
            else
            {
                const Term t = term_sum( x[i++], y[j++] );
                if( !zero( t ) )
                {
                    out.push_back( t );
                }
            }
        }
        return form( std::move( out ) );
    }

    Term negative_one()
    {
        return { .bits = wrap( ~u64 { 0 } ), .exact = integer_ ? Exact {} : rational( -1 ) };
    }

    Form scaled( const Form& x, const Term& c )
    {
        Form out;
        if( zero( c ) )
        {
            return out;
        }
        for( Term t : x )
        {
            t.bits  = wrap( t.bits * c.bits );
            t.exact = integer_ ? Exact {} : simplest( product( t.exact, c.exact ) );
            out.push_back( t );
        }
        return out;
    }

    // A number as a coefficient, or the attempt ends.
    Term coefficient( Value n, bool inverted )
    {
        if( n.kind == Kind::Unknown || n.kind == Kind::Address )
        {
            end( Ending::Unknown );
        }
        if( integer_ )
        {
            return { .bits = wrap( n.bits ) };
        }
        std::optional<Exact> e = exact_of( n );
        if( e && inverted )
        {
            e = inverse( *e );
        }
        if( !e )
        {
            end( Ending::Coefficient );
        }
        return { .exact = *e };
    }

    // A number as a form, its coefficient on the input fixed at 1.
    Value constant_form( Value n )
    {
        if( is_zero( n ) )
        {
            return form( {} );
        }
        const Term c = coefficient( n, false );
        if( one_ == ~0u )
        {
            one_ = static_cast<u32>( inputs_.size() );
            inputs_.push_back( { .root = k_one_root, .steps = {} } );
        }
        return form( { { .input = one_, .bits = c.bits, .exact = c.exact } } );
    }

    bool is_zero( Value n ) const
    {
        return n.kind == Kind::Number && ( integer_ ? wrap( n.bits ) == 0 : as_real( n ) == 0.0 );
    }

    // Memory.

    Frame frame( const Function& func )
    {
        return { &func, std::vector<u32>( func.locals.size(), ~0u ) };
    }

    u32 root( Frame& fr, Local_id local )
    {
        u32& r = fr.roots[local.v];
        if( r == ~0u )
        {
            r = static_cast<u32>( roots_.size() );
            roots_.emplace_back();
        }
        return r;
    }

    u32 address_of( Location at )
    {
        locations_.push_back( std::move( at ) );
        return static_cast<u32>( locations_.size() - 1 );
    }

    Value input( const Location& at, Type_id type )
    {
        if( !ring_ )
        {
            ring_    = true;
            type_    = type;
            integer_ = !is_float( type );
            width_   = types_.get( type ).width;
        }
        else if( type != type_ )
        {
            end( Ending::Mixed );
        }
        const u32 j = static_cast<u32>( inputs_.size() );
        inputs_.push_back( at );
        const Value v = form( { { .input = j, .bits = 1, .exact = integer_ ? Exact {} : rational( 1 ) } } );
        read_.emplace( at, v );
        return v;
    }

    Value read( const Location& at, Type_id type )
    {
        if( const auto found = memory_.find( at ); found != memory_.end() )
        {
            return found->second.value;
        }
        if( symbolic_ && roots_[at.root].input )
        {
            return stream_read( at, type );
        }
        if( const auto found = read_.find( at ); found != read_.end() )
        {
            return found->second;
        }
        const Type_kind kind = types_.get( type ).kind;
        if( roots_[at.root].input && ( kind == Type_kind::Int || is_f64( type ) ) )
        {
            return input( at, type );
        }
        return {};
    }

    void write( const Location& at, Value v )
    {
        if( symbolic_ && roots_[at.root].output )
        {
            stream_write( at, v );
            return;
        }
        if( at.poly != 0 )
        {
            end( Ending::Unsupported );
        }
        memory_[at] = { v, span_ };
    }

    static bool under( const Location& at, const Location& prefix )
    {
        return at.root == prefix.root && at.steps.size() >= prefix.steps.size() &&
               std::equal( prefix.steps.begin(), prefix.steps.end(), at.steps.begin() );
    }

    // Everything written under `from`, now under `to` instead of what was there.
    void copy_whole( const Location& to, const Location& from )
    {
        if( roots_[from.root].input || ( symbolic_ && roots_[to.root].output ) || from.poly != 0 || to.poly != 0 )
        {
            end( Ending::Unsupported );
        }
        std::vector<std::pair<std::vector<u64>, Cell>> parts;
        for( auto it = memory_.lower_bound( from ); it != memory_.end() && under( it->first, from ); ++it )
        {
            parts.emplace_back(
                std::vector<u64>(
                    it->first.steps.begin() + static_cast<std::ptrdiff_t>( from.steps.size() ), it->first.steps.end()
                ),
                it->second
            );
        }
        auto it = memory_.lower_bound( to );
        while( it != memory_.end() && under( it->first, to ) )
        {
            it = memory_.erase( it );
        }
        for( auto& [suffix, cell] : parts )
        {
            Location at = to;
            at.steps.insert( at.steps.end(), suffix.begin(), suffix.end() );
            memory_[at] = cell;
        }
    }

    // A constant array's root, its elements written there on first use.
    u32 table( const Rvalue& r )
    {
        const auto [found, added] = table_roots_.try_emplace( r.a.constant.v, static_cast<u32>( roots_.size() ) );
        if( added )
        {
            roots_.emplace_back();
            const Type_id          element = types_.get( types_.get( r.type ).element ).element;
            const u64              size    = types_.get( element ).width / 8;
            const std::string_view bytes   = literals_.string( r.a.constant );
            for( u64 at = 0; at < bytes.size(); at += size )
            {
                u64 bits = 0;
                std::memcpy( &bits, bytes.data() + at, size );
                f64 d = 0;
                std::memcpy( &d, &bits, sizeof d );
                write(
                    { found->second, { at / size << 2 } },
                    is_f64( element )     ? real( d, exact_literal( d ) )
                    : is_float( element ) ? Value {}
                                          : number( bits )
                );
            }
        }
        return found->second;
    }

    // A `const` global's root, its value written there on first use.
    std::pair<u32, Type_id> global( Node_id decl )
    {
        if( const auto found = global_roots_.find( decl.v ); found != global_roots_.end() )
        {
            return found->second;
        }
        const std::optional<Global_constant> c = globals_ ? globals_( decl ) : std::nullopt;
        if( !c )
        {
            end( Ending::Global );
        }
        const u32 r = static_cast<u32>( roots_.size() );
        roots_.emplace_back();
        store( { r, {} }, c->value, c->type );
        return global_roots_.emplace( decl.v, std::pair { r, c->type } ).first->second;
    }

    // Numbers as a literal of their type would read; anything else stays unknown.
    void store( const Location& at, const Constant_value& v, Type_id type )
    {
        const Type& t = types_.get( type );
        if( v.kind == Constant_value::Kind::Array || v.kind == Constant_value::Kind::Fill )
        {
            for( u64 i = 0; i < t.count; ++i )
            {
                Location e = at;
                e.steps.push_back( i << 2 );
                store( e, v.elements[v.kind == Constant_value::Kind::Fill ? 0 : i], t.element );
            }
        }
        else if( is_f64( type ) && ( v.kind == Constant_value::Kind::Float || v.kind == Constant_value::Kind::Integer ) )
        {
            const f64 x = v.kind == Constant_value::Kind::Float ? v.floating
                                                                : ( v.negative ? -1.0 : 1.0 ) * static_cast<f64>( v.magnitude );
            write( at, real( x, exact_literal( x ) ) );
        }
        else if( t.kind == Type_kind::Int && v.kind == Constant_value::Kind::Integer )
        {
            write( at, number( v.negative ? 0 - v.magnitude : v.magnitude ) );
        }
    }

    Location locate( Frame& fr, const Place& place )
    {
        Location at;
        Type_id  type;
        if( place.is_global() )
        {
            std::tie( at.root, type ) = global( place.global );
        }
        else
        {
            at   = { root( fr, place.local ), {} };
            type = fr.func->locals[place.local].type;
        }
        for( const Projection& p : fr.func->projections_of( place ) )
        {
            switch( p.kind )
            {
            case Projection_kind::Field:
                at.steps.push_back( u64 { p.field.v } << 2 | 1 );
                break;
            case Projection_kind::Index:
            {
                const Value i = read( Location { root( fr, p.index ), {} }, fr.func->locals[p.index].type );
                if( i.kind == Kind::Int )
                {
                    // Only a parameter's elements are taken at a symbolic index.
                    const sym::Int& x = symbolics_[i.index].i;
                    if( at.poly != 0 || !roots_[at.root].input || x.q != 0 || x.lo < 0 )
                    {
                        end( Ending::Branch );
                    }
                    polys_.push_back( x.p );
                    at.poly = static_cast<u32>( polys_.size() - 1 );
                    at.steps.push_back( k_symbolic_step );
                    break;
                }
                if( i.kind != Kind::Number )
                {
                    end( Ending::Branch );
                }
                at.steps.push_back( i.bits << 2 );
                break;
            }
            case Projection_kind::Deref:
            {
                const Value a = read( at, type );
                if( a.kind != Kind::Address )
                {
                    end( Ending::Unsupported );
                }
                at = locations_[a.index];
                break;
            }
            case Projection_kind::Tag:
            case Projection_kind::Member:
                end( Ending::Unsupported );
            }
        }
        return at;
    }

    // Execution.

    void spend()
    {
        if( ++steps_ > budget_ )
        {
            end( Ending::Budget );
        }
    }

    Value constant( const Operand& o ) const
    {
        if( is_float( o.type ) )
        {
            return is_f64( o.type ) ? real( literals_.floating( o.constant ) ) : Value {};
        }
        return literals_.kind( o.constant ) == Literal_kind::Integer ? number( literals_.integer( o.constant ) ) : Value {};
    }

    Value operand( Frame& fr, const Operand& o )
    {
        switch( o.kind )
        {
        case Operand_kind::Constant:
        {
            Value v = constant( o );
            if( v.kind == Kind::Number && is_f64( o.type ) )
            {
                v = real( as_real( v ), exact_literal( as_real( v ) ) );
            }
            return v;
        }
        case Operand_kind::Copy:
        case Operand_kind::Move:
            return read( locate( fr, o.place ), o.type );
        case Operand_kind::Debug:
            return {};
        }
        return {};
    }

    void count( bool multiply )
    {
        ( multiply ? cost_.muls : cost_.adds )++;
    }

    Value symbolic( Token_kind op, Value a, Value b )
    {
        switch( op )
        {
        case Token_kind::Plus:
        case Token_kind::Minus:
        {
            if( a.kind == Kind::Form && b.kind == Kind::Form )
            {
                count( false );
                return combined( a, b, op == Token_kind::Minus );
            }
            const bool  left = a.kind == Kind::Form;
            const Value n    = left ? b : a;
            if( n.kind != Kind::Number )
            {
                end( Ending::Unknown );
            }
            count( false );
            const Value k = constant_form( n );
            return left ? combined( a, k, op == Token_kind::Minus ) : combined( k, b, op == Token_kind::Minus );
        }
        case Token_kind::Star:
        {
            if( a.kind == Kind::Form && b.kind == Kind::Form )
            {
                end( Ending::Product );
            }
            const bool left = a.kind == Kind::Form;
            const Term c    = coefficient( left ? b : a, false );
            count( true );
            return form( scaled( forms_[( left ? a : b ).index], c ) );
        }
        case Token_kind::Slash:
        {
            if( b.kind == Kind::Form || integer_ )
            {
                end( Ending::Division );
            }
            const Term c = coefficient( b, true );
            count( true );
            return form( scaled( forms_[a.index], c ) );
        }
        case Token_kind::Percent:
            end( Ending::Remainder );
        case Token_kind::Less_less:
        case Token_kind::Greater_greater:
            end( Ending::Shift );
        case Token_kind::Amp:
        case Token_kind::Pipe:
        case Token_kind::Caret:
            end( Ending::Bits );
        case Token_kind::Equal_equal:
        case Token_kind::Bang_equal:
        case Token_kind::Less:
        case Token_kind::Greater:
        case Token_kind::Less_equal:
        case Token_kind::Greater_equal:
        case Token_kind::Less_equal_greater:
            end( Ending::Compare );
        default:
            end( Ending::Unsupported );
        }
    }

    Value real_binary( Token_kind op, Value a, Value b )
    {
        const f64            x  = as_real( a );
        const f64            y  = as_real( b );
        const auto           ea = exact_of( a );
        const auto           eb = exact_of( b );
        std::optional<Exact> e;
        switch( op )
        {
        case Token_kind::Plus:
            if( ea && eb )
            {
                e = simplest( sum( *ea, *eb ) );
            }
            return real( x + y, e );
        case Token_kind::Minus:
            if( ea && eb )
            {
                e = simplest( sum( *ea, negated( *eb ) ) );
            }
            return real( x - y, e );
        case Token_kind::Star:
            if( ea && eb )
            {
                e = simplest( product( *ea, *eb ) );
            }
            return real( x * y, e );
        case Token_kind::Slash:
            if( ea && eb )
            {
                if( const std::optional<Exact> inv = inverse( *eb ) )
                {
                    e = simplest( product( *ea, *inv ) );
                }
            }
            return real( x / y, e );
        case Token_kind::Equal_equal:
            return number( x == y );
        case Token_kind::Bang_equal:
            return number( x != y );
        case Token_kind::Less:
            return number( x < y );
        case Token_kind::Greater:
            return number( x > y );
        case Token_kind::Less_equal:
            return number( x <= y );
        case Token_kind::Greater_equal:
            return number( x >= y );
        default:
            return {};
        }
    }

    Value binary( const Rvalue& r, Value a, Value b )
    {
        if( is_symbolic( a ) || is_symbolic( b ) )
        {
            return sym_binary( r, a, b );
        }
        if( a.kind == Kind::Form || b.kind == Kind::Form )
        {
            return symbolic( r.op, a, b );
        }
        if( a.kind != Kind::Number || b.kind != Kind::Number )
        {
            return {};
        }
        if( is_float( r.a.type ) )
        {
            return is_f64( r.a.type ) ? real_binary( r.op, a, b ) : Value {};
        }
        const std::optional<u64> bits = evaluate( r, a.bits, b.bits, types_ );
        return bits ? number( *bits ) : Value {};
    }

    Value unary( const Rvalue& r, Value a )
    {
        if( is_symbolic( a ) )
        {
            return sym_unary( r, a );
        }
        if( a.kind == Kind::Form )
        {
            if( r.op != Token_kind::Minus )
            {
                end( Ending::Bits );
            }
            return form( scaled( forms_[a.index], negative_one() ) );
        }
        if( a.kind != Kind::Number )
        {
            return {};
        }
        if( is_float( r.a.type ) )
        {
            const auto e = exact_of( a );
            return is_f64( r.a.type ) && r.op == Token_kind::Minus
                       ? real( -as_real( a ), e ? std::optional( negated( *e ) ) : std::nullopt )
                       : Value {};
        }
        const std::optional<u64> bits = evaluate( r, a.bits, std::nullopt, types_ );
        return bits ? number( *bits ) : Value {};
    }

    Value cast( const Rvalue& r, Value a )
    {
        if( is_symbolic( a ) )
        {
            return sym_cast( r, a );
        }
        if( a.kind == Kind::Form )
        {
            if( r.type != r.a.type )
            {
                end( Ending::Cast );
            }
            return a;
        }
        if( a.kind != Kind::Number )
        {
            return {};
        }
        const Type& from = types_.get( r.a.type );
        const Type& to   = types_.get( r.type );
        if( from.kind == Type_kind::Int && to.kind == Type_kind::Bool )
        {
            return number( a.bits != 0 );
        }
        if( from.kind == Type_kind::Int && is_f64( r.type ) )
        {
            const bool fits = from.is_signed || a.bits < ( u64 { 1 } << 63 );
            const i64  v    = from.is_signed ? as_signed( a.bits, from.width ) : static_cast<i64>( a.bits );
            const f64  x    = from.is_signed ? static_cast<f64>( v ) : static_cast<f64>( a.bits );
            return real( x, fits ? std::optional( rational( v ) ) : std::nullopt );
        }
        if( is_f64( r.a.type ) && to.kind == Type_kind::Int )
        {
            const f64 x  = std::trunc( as_real( a ) );
            const f64 lo = to.is_signed ? -std::ldexp( 1.0, to.width - 1 ) : 0.0;
            const f64 hi = std::ldexp( 1.0, to.is_signed ? to.width - 1 : to.width );
            if( !( x >= lo && x < hi ) )
            {
                return {};
            }
            return number( truncate( x < 0 ? static_cast<u64>( static_cast<i64>( x ) ) : static_cast<u64>( x ), to.width ) );
        }
        if( is_f64( r.a.type ) && is_f64( r.type ) )
        {
            return a;
        }
        const std::optional<u64> bits = evaluate( r, a.bits, std::nullopt, types_ );
        return bits ? number( *bits ) : Value {};
    }

    Value trig( Frame& fr, const Rvalue& r, Trig kind )
    {
        if( r.arguments.size() != 1 )
        {
            end( Ending::Unsupported );
        }
        Value x = operand( fr, r.arguments[0] );
        if( x.kind == Kind::Address )
        {
            x = read( locations_[x.index], types_.floating( 64 ) );
        }
        if( x.kind == Kind::Form )
        {
            end( Ending::Unsupported );
        }
        if( x.kind != Kind::Number )
        {
            return {};
        }
        const f64 a = as_real( x );
        const f64 y = kind == Trig::Sin ? std::sin( a ) : kind == Trig::Cos ? std::cos( a ) : std::tan( a );
        return real( y, exact_trig( kind, a ) );
    }

    // Runs a call, its result written to `to`.
    void call( Frame& fr, const Rvalue& r, const Location& to )
    {
        const std::optional<u32> c = purity_.callee( r );
        if( !c )
        {
            end( Ending::Unsupported );
        }
        const Function& g = functions_[*c];
        if( g.trig != Trig::None )
        {
            write( to, symbolic_ ? sym_trig( fr, r, g.trig ) : trig( fr, r, g.trig ) );
            return;
        }
        if( !purity_.pure( r ) )
        {
            bool symbolic = false;
            for( const Operand& argument : r.arguments )
            {
                symbolic = symbolic || linear( operand( fr, argument ) );
            }
            end( symbolic ? Ending::Escape : Ending::Effect );
        }
        if( depth_ >= k_deepest_call || g.parameter_count != r.arguments.size() )
        {
            end( Ending::Budget );
        }

        Frame callee = frame( g );
        for( u32 i = 0; i < r.arguments.size(); ++i )
        {
            const Operand& argument = r.arguments[i];
            const Location at { root( callee, Local_id { i + 1 } ), {} };
            if( is_aggregate( argument.type ) && argument.has_place() )
            {
                copy_whole( at, locate( fr, argument.place ) );
            }
            else
            {
                write( at, operand( fr, argument ) );
            }
        }
        const Span span = span_;
        execute( callee );
        span_ = span;

        const Location result { root( callee, k_return_slot ), {} };
        if( is_aggregate( r.type ) )
        {
            copy_whole( to, result );
        }
        else if( types_.get( r.type ).kind != Type_kind::Void )
        {
            write( to, read( result, r.type ) );
        }
    }

    Value value( Frame& fr, const Rvalue& r )
    {
        switch( r.kind )
        {
        case Rvalue_kind::Use:
            return operand( fr, r.a );
        case Rvalue_kind::Binary:
            return binary( r, operand( fr, r.a ), operand( fr, r.b ) );
        case Rvalue_kind::Unary:
            return unary( r, operand( fr, r.a ) );
        case Rvalue_kind::Overflows:
        {
            const Value a = operand( fr, r.a );
            const Value b = operand( fr, r.b );
            if( linear( a ) || linear( b ) )
            {
                end( Ending::Unsupported );
            }
            if( a.kind != Kind::Number || b.kind != Kind::Number )
            {
                return {};
            }
            const std::optional<u64> bits = evaluate( r, a.bits, b.bits, types_ );
            return bits ? number( *bits ) : Value {};
        }
        case Rvalue_kind::Cast:
            return cast( r, operand( fr, r.a ) );
        case Rvalue_kind::Bits:
        {
            const Value a = operand( fr, r.a );
            if( linear( a ) )
            {
                end( Ending::Bits );
            }
            return a.kind == Kind::Number ? number( a.bits ) : Value {};
        }
        case Rvalue_kind::Sqrt:
        {
            const Value a = operand( fr, r.a );
            if( linear( a ) )
            {
                end( Ending::Unsupported );
            }
            if( a.kind != Kind::Number || !is_f64( r.a.type ) )
            {
                return {};
            }
            const auto e = exact_of( a );
            return real( std::sqrt( as_real( a ) ), e ? exact_root( *e ) : std::nullopt );
        }
        case Rvalue_kind::Address_of:
            return { .kind = Kind::Address, .index = address_of( locate( fr, r.a.place ) ) };
        case Rvalue_kind::Constant_array:
            return { .kind = Kind::Address, .index = address_of( { table( r ), {} } ) };
        default:
            end( Ending::Unsupported );
        }
    }

    void assign( Frame& fr, const Statement& s )
    {
        const Rvalue& r = fr.func->rvalue( s );
        if( s.place.is_global() )
        {
            const Value v = r.kind == Rvalue_kind::Call ? Value {} : value( fr, r );
            end( linear( v ) ? Ending::Escape : Ending::Effect );
        }
        if( r.kind == Rvalue_kind::Call )
        {
            call( fr, r, locate( fr, s.place ) );
            return;
        }
        if( r.kind == Rvalue_kind::Use && is_aggregate( r.type ) && r.a.has_place() )
        {
            const Location from = locate( fr, r.a.place );
            copy_whole( locate( fr, s.place ), from );
            return;
        }
        const Value v = value( fr, r );
        write( locate( fr, s.place ), v );
    }

    void statement( Frame& fr, const Statement& s )
    {
        spend();
        span_ = s.span;
        if( s.kind == Statement_kind::Assign )
        {
            assign( fr, s );
        }
        else if( s.kind == Statement_kind::Drop )
        {
            end( Ending::Unsupported );
        }
    }

    void execute( Frame& fr )
    {
        ++depth_;
        Block_id b = k_entry_block;
        for( ;; )
        {
            for( const Statement& s : fr.func->statements_of( b ) )
            {
                statement( fr, s );
            }
            spend();
            const Terminator& t = fr.func->blocks[b].terminator;
            span_               = t.span;
            switch( t.kind )
            {
            case Terminator_kind::Goto:
                b = t.targets[0];
                break;
            case Terminator_kind::Branch:
            {
                const Value c = operand( fr, t.condition );
                if( c.kind != Kind::Number )
                {
                    end( Ending::Branch, t.condition.span );
                }
                b = t.targets[c.bits != 0 ? 0 : 1];
                break;
            }
            case Terminator_kind::Return:
                --depth_;
                return;
            case Terminator_kind::Unreachable:
            case Terminator_kind::Panic:
            case Terminator_kind::Unset:
                end( Ending::Panic );
            }
        }
    }

    // Symbolic mode: each counted loop of the function read runs once with its index a symbol.

    static bool is_symbolic( Value v )
    {
        return v.kind >= Kind::Int;
    }

    static bool linear( Value v )
    {
        return v.kind == Kind::Form || is_symbolic( v );
    }

    Value make( Kind kind, Symbolic s )
    {
        symbolics_.push_back( std::move( s ) );
        return { .kind = kind, .index = static_cast<u32>( symbolics_.size() - 1 ) };
    }

    Value angle( sym::Int i, f64 a, f64 b )
    {
        return make( Kind::Angle, { .i = std::move( i ), .a = a, .b = b } );
    }

    static sym::Poly scaled_poly( const sym::Poly& p, i64 c )
    {
        return sym::product( p, sym::constant( c ) );
    }

    // (a i + b)(a' i' + b') with b/a = n/d and b'/a' = n'/d': a a'/(d d') (d i + n)(d' i' + n').
    Value angle_product( Symbolic s, Symbolic t )
    {
        const auto u = s.i.q == 0 && s.a != 0 ? sym::rational( s.b / s.a ) : std::nullopt;
        const auto w = t.i.q == 0 && t.a != 0 ? sym::rational( t.b / t.a ) : std::nullopt;
        if( !u || !w )
        {
            sym::fail();
        }
        const i64       d = static_cast<i64>( u->second ), e = static_cast<i64>( w->second );
        const sym::Poly p = sym::product(
            sym::sum( scaled_poly( s.i.p, d ), sym::constant( u->first ) ),
            sym::sum( scaled_poly( t.i.p, e ), sym::constant( w->first ) )
        );
        return angle( made( p, 0, 0, 0 ), s.a * t.a / static_cast<f64>( d * e ), 0 );
    }

    Value terms( sym::Form f )
    {
        return make( Kind::Terms, { .f = std::move( f ) } );
    }

    u8 width_of( Type_id type ) const
    {
        return types_.is_integer( type ) ? types_.get( type ).width : 64;
    }

    // `x` as a value of integer `type`: a number once it is one.
    Value integer( sym::Int x, Type_id type )
    {
        if( const std::optional<i64> c = sym::constant_of( x.p ); c && x.q == 0 )
        {
            return number( truncate( static_cast<u64>( *c ), width_of( type ) ) );
        }
        return make( Kind::Int, { .i = std::move( x ) } );
    }

    sym::Int int_of( Value v, Type_id type ) const
    {
        if( v.kind == Kind::Int )
        {
            return symbolics_[v.index].i;
        }
        if( v.kind != Kind::Number || is_float( type ) )
        {
            sym::fail();
        }
        const bool is_signed = types_.is_integer( type ) && types_.get( type ).is_signed;
        if( !is_signed && v.bits > static_cast<u64>( std::numeric_limits<i64>::max() ) )
        {
            sym::fail();
        }
        return sym::number( is_signed ? as_signed( v.bits, width_of( type ) ) : static_cast<i64>( v.bits ) );
    }

    bool depends( Value v, u32 s ) const
    {
        switch( v.kind )
        {
        case Kind::Int:
        case Kind::Angle:
            return sym::depends( symbolics_[v.index].i.p, s );
        case Kind::Wave:
            return sym::depends( symbolics_[v.index].w, s );
        case Kind::Terms:
            return std::ranges::any_of(
                symbolics_[v.index].f.terms,
                [s]( const auto& t ) {
                    return ( ( t.first.bound >> s & 1 ) == 0 && sym::depends( t.first.index, s ) ) ||
                           sym::depends( t.second, s );
                }
            );
        default:
            return false;
        }
    }

    // The range a value of integer `type` may take.
    std::pair<i128, i128> limits( Type_id type ) const
    {
        const Type& t = types_.get( type );
        if( t.is_signed )
        {
            return { -( i128 { 1 } << ( t.width - 1 ) ), ( i128 { 1 } << ( t.width - 1 ) ) - 1 };
        }
        return { 0, ( i128 { 1 } << t.width ) - 1 };
    }

    sym::Int within( sym::Int x, Type_id type ) const
    {
        const auto [lo, hi] = limits( type );
        if( x.lo < lo || x.hi > hi )
        {
            sym::fail();
        }
        return x;
    }

    // The exact range of p over the symbols' ranges, which start at 0.
    std::pair<i128, i128> range( const sym::Poly& p ) const
    {
        i128 lo = 0, hi = 0;
        for( const auto& [m, c] : p )
        {
            i128 top = 1;
            for( u32 s = 0; s < sym::k_symbols; ++s )
            {
                for( u8 e = 0; e < m[s]; ++e )
                {
                    top *= static_cast<i128>( turns_[s] ) - 1;
                    if( top > ( i128 { 1 } << 64 ) )
                    {
                        sym::fail();
                    }
                }
            }
            if( m == sym::Monomial {} )
            {
                lo += c;
                hi += c;
            }
            else if( c > 0 )
            {
                hi += c * top;
            }
            else
            {
                lo += c * top;
            }
        }
        return { lo, hi };
    }

    sym::Int made( sym::Poly p, u64 q, i128 lo, i128 hi ) const
    {
        if( q == 0 )
        {
            const auto [l, h] = range( p );
            return { std::move( p ), 0, l, h };
        }
        return { std::move( p ), q, lo, hi };
    }

    Value sym_binary( const Rvalue& r, Value x, Value y )
    {
        if( compares( r.op ) )
        {
            return compare( r.op, x, y, r.a.type );
        }
        if( is_float( r.a.type ) )
        {
            return arithmetic( r.op, x, y );
        }
        const sym::Int a = int_of( x, r.a.type );
        const sym::Int b = int_of( y, r.b.type );
        const u64      q = std::gcd( a.q, b.q );
        switch( r.op )
        {
        case Token_kind::Plus:
            return integer( within( made( sym::sum( a.p, b.p ), q, a.lo + b.lo, a.hi + b.hi ), r.type ), r.type );
        case Token_kind::Minus:
            return integer( within( made( sym::sum( a.p, b.p, -1 ), q, a.lo - b.hi, a.hi - b.lo ), r.type ), r.type );
        case Token_kind::Star:
        {
            const i128 c[] { a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi };
            return integer(
                within(
                    made( sym::product( a.p, b.p ), q, *std::ranges::min_element( c ), *std::ranges::max_element( c ) ), r.type
                ),
                r.type
            );
        }
        case Token_kind::Percent:
        {
            if( !sym::concrete( b ) || *sym::constant_of( b.p ) <= 0 || a.lo < 0 )
            {
                sym::fail();
            }
            const i64 n = *sym::constant_of( b.p );
            if( a.hi < n )
            {
                return x;
            }
            return integer(
                { a.p,
                  a.q == 0 ? static_cast<u64>( n ) : std::gcd( a.q, static_cast<u64>( n ) ),
                  0,
                  std::min<i128>( n - 1, a.hi ) },
                r.type
            );
        }
        default:
            sym::fail();
        }
    }

    static bool compares( Token_kind op )
    {
        switch( op )
        {
        case Token_kind::Less:
        case Token_kind::Less_equal:
        case Token_kind::Greater:
        case Token_kind::Greater_equal:
        case Token_kind::Equal_equal:
        case Token_kind::Bang_equal:
            return true;
        default:
            return false;
        }
    }

    // [lo, hi] of an integer or a real value.
    std::optional<std::pair<f64, f64>> span_of( Value v, Type_id type ) const
    {
        switch( v.kind )
        {
        case Kind::Int:
            return std::pair { static_cast<f64>( symbolics_[v.index].i.lo ), static_cast<f64>( symbolics_[v.index].i.hi ) };
        case Kind::Number:
        {
            const f64 x = is_float( type ) ? as_real( v ) : static_cast<f64>( int_of( v, type ).lo );
            return std::pair { x, x };
        }
        case Kind::Angle:
        {
            const Symbolic& s = symbolics_[v.index];
            const f64       l = s.a * static_cast<f64>( s.i.lo ) + s.b;
            const f64       h = s.a * static_cast<f64>( s.i.hi ) + s.b;
            return std::pair { std::min( l, h ), std::max( l, h ) };
        }
        default:
            return std::nullopt;
        }
    }

    // Decided from the ranges, or by being the same value.
    Value compare( Token_kind op, Value x, Value y, Type_id type ) const
    {
        const auto same = [&]
        {
            if( x.kind != y.kind || ( x.kind != Kind::Int && x.kind != Kind::Angle ) )
            {
                return false;
            }
            const Symbolic& a = symbolics_[x.index];
            const Symbolic& b = symbolics_[y.index];
            return sym::same( a.i, b.i ) && a.a == b.a && a.b == b.b;
        };
        const bool equal = same();
        const auto sx    = span_of( x, type );
        const auto sy    = span_of( y, type );
        if( !sx || !sy )
        {
            return {};
        }
        const auto [xl, xh] = *sx;
        const auto [yl, yh] = *sy;
        switch( op )
        {
        case Token_kind::Less:
            return xh < yl ? number( 1 ) : xl >= yh ? number( 0 ) : Value {};
        case Token_kind::Less_equal:
            return xh <= yl ? number( 1 ) : xl > yh ? number( 0 ) : Value {};
        case Token_kind::Greater:
            return compare( Token_kind::Less, y, x, type );
        case Token_kind::Greater_equal:
            return compare( Token_kind::Less_equal, y, x, type );
        case Token_kind::Equal_equal:
            return equal ? number( 1 ) : xh < yl || yh < xl ? number( 0 ) : Value {};
        case Token_kind::Bang_equal:
            return equal ? number( 0 ) : xh < yl || yh < xl ? number( 1 ) : Value {};
        default:
            return {};
        }
    }

    // An exact number as powers of W_m with rational coefficients.
    sym::Wave wave_of( std::optional<Exact> a )
    {
        if( !a )
        {
            sym::fail();
        }
        Field&    f = field( a->m );
        sym::Wave w { a->m, {} };
        for( u32 k = 0; k < f.degree(); ++k )
        {
            if( const auto [n, d] = f.coefficient( a->e, k ); n != 0 )
            {
                sym::add_term( w, sym::constant( k ), Rational( n, d ) );
            }
        }
        return w;
    }

    sym::Wave as_wave( Value v )
    {
        if( v.kind == Kind::Number )
        {
            return wave_of( exact_of( v ) );
        }
        if( v.kind != Kind::Wave )
        {
            sym::fail();
        }
        return symbolics_[v.index].w;
    }

    sym::Form as_form( Value v )
    {
        if( v.kind == Kind::Terms )
        {
            return symbolics_[v.index].f;
        }
        if( v.kind == Kind::Number && as_real( v ) == 0 )
        {
            return {};
        }
        if( v.kind == Kind::Number || v.kind == Kind::Wave )
        {
            return { .terms = { { sym::Key { sym::k_one_stream, {}, 0 }, as_wave( v ) } } };
        }
        sym::fail();
    }

    Value add( Value x, Value y, i64 sign )
    {
        if( x.kind == Kind::Terms || y.kind == Kind::Terms )
        {
            return terms( sym::sum( as_form( x ), as_form( y ), sign ) );
        }
        if( x.kind == Kind::Angle || y.kind == Kind::Angle )
        {
            const f64 r = static_cast<f64>( sign );
            if( x.kind == Kind::Angle && y.kind == Kind::Number )
            {
                const Symbolic& s = symbolics_[x.index];
                return angle( s.i, s.a, s.b + r * as_real( y ) );
            }
            if( x.kind == Kind::Number && y.kind == Kind::Angle )
            {
                const Symbolic& s = symbolics_[y.index];
                return angle( s.i, r * s.a, as_real( x ) + r * s.b );
            }
            if( x.kind == Kind::Angle && y.kind == Kind::Angle && sym::same( symbolics_[x.index].i, symbolics_[y.index].i ) )
            {
                const Symbolic& s = symbolics_[x.index];
                const Symbolic& t = symbolics_[y.index];
                return angle( s.i, s.a + r * t.a, s.b + r * t.b );
            }
            // a i + a p/q i' = a/q (q i + p i').
            if( x.kind == Kind::Angle && y.kind == Kind::Angle )
            {
                const Symbolic s = symbolics_[x.index];
                const Symbolic t = symbolics_[y.index];
                if( const auto ratio = s.i.q == 0 && t.i.q == 0 && s.a != 0 ? sym::rational( t.a / s.a ) : std::nullopt )
                {
                    const i64 q = static_cast<i64>( ratio->second );
                    return angle(
                        made( sym::sum( scaled_poly( s.i.p, q ), scaled_poly( t.i.p, ratio->first ), sign ), 0, 0, 0 ),
                        s.a / static_cast<f64>( q ),
                        s.b + r * t.b
                    );
                }
            }
            sym::fail();
        }
        return make( Kind::Wave, { .w = sym::sum( as_wave( x ), as_wave( y ), sign ) } );
    }

    Value arithmetic( Token_kind op, Value x, Value y )
    {
        Value v;
        switch( op )
        {
        case Token_kind::Plus:
        case Token_kind::Minus:
            v = add( x, y, op == Token_kind::Plus ? 1 : -1 );
            break;
        case Token_kind::Star:
            if( x.kind == Kind::Terms && y.kind == Kind::Terms )
            {
                sym::fail();
            }
            if( x.kind == Kind::Terms || y.kind == Kind::Terms )
            {
                v = x.kind == Kind::Terms ? terms( sym::scaled( symbolics_[x.index].f, as_wave( y ) ) )
                                          : terms( sym::scaled( symbolics_[y.index].f, as_wave( x ) ) );
            }
            else if( x.kind == Kind::Angle && y.kind == Kind::Number )
            {
                const Symbolic& s = symbolics_[x.index];
                v                 = angle( s.i, s.a * as_real( y ), s.b * as_real( y ) );
            }
            else if( x.kind == Kind::Number && y.kind == Kind::Angle )
            {
                const Symbolic& s = symbolics_[y.index];
                v                 = angle( s.i, as_real( x ) * s.a, as_real( x ) * s.b );
            }
            else if( x.kind == Kind::Angle && y.kind == Kind::Angle )
            {
                v = angle_product( symbolics_[x.index], symbolics_[y.index] );
            }
            else
            {
                v = make( Kind::Wave, { .w = sym::product( as_wave( x ), as_wave( y ) ) } );
            }
            break;
        case Token_kind::Slash:
        {
            if( y.kind != Kind::Number || as_real( y ) == 0 )
            {
                sym::fail();
            }
            const f64       d       = as_real( y );
            const auto      e       = exact_of( y );
            const sym::Wave reverse = wave_of( e ? inverse( *e ) : std::nullopt );
            if( x.kind == Kind::Angle )
            {
                const Symbolic& s = symbolics_[x.index];
                v                 = angle( s.i, s.a / d, s.b / d );
            }
            else if( x.kind == Kind::Terms )
            {
                v = terms( sym::scaled( symbolics_[x.index].f, reverse ) );
            }
            else
            {
                v = make( Kind::Wave, { .w = sym::product( as_wave( x ), reverse ) } );
            }
            break;
        }
        default:
            sym::fail();
        }
        if( v.kind == Kind::Terms )
        {
            ( op == Token_kind::Star || op == Token_kind::Slash ? muls_ : adds_ ) += multiplicity_;
        }
        return v;
    }

    Value sym_unary( const Rvalue& r, Value x )
    {
        if( r.op != Token_kind::Minus )
        {
            sym::fail();
        }
        const Symbolic& s = symbolics_[x.index];
        switch( x.kind )
        {
        case Kind::Int:
            return integer( within( made( sym::sum( {}, s.i.p, -1 ), s.i.q, -s.i.hi, -s.i.lo ), r.type ), r.type );
        case Kind::Angle:
            return angle( s.i, -s.a, -s.b );
        case Kind::Wave:
            return make( Kind::Wave, { .w = sym::scaled( s.w, -1 ) } );
        case Kind::Terms:
            return terms( sym::scaled( s.f, sym::wave( -1 ) ) );
        default:
            sym::fail();
        }
    }

    Value sym_cast( const Rvalue& r, Value x )
    {
        const bool from_int = types_.is_integer( r.a.type );
        const bool to_int   = types_.is_integer( r.type );
        if( from_int && to_int && x.kind == Kind::Int )
        {
            return integer( within( symbolics_[x.index].i, r.type ), r.type );
        }
        if( from_int && is_f64( r.type ) && x.kind == Kind::Int )
        {
            const sym::Int& i     = symbolics_[x.index].i;
            const i128      exact = i128 { 1 } << 53;
            if( i.lo < -exact || i.hi > exact )
            {
                sym::fail();
            }
            return angle( i, 1, 0 );
        }
        if( to_int && x.kind == Kind::Angle && symbolics_[x.index].a == 1 && symbolics_[x.index].b == 0 )
        {
            return integer( within( symbolics_[x.index].i, r.type ), r.type );
        }
        if( r.a.type == r.type )
        {
            return x;
        }
        sym::fail();
    }

    // A `@trig_*` call at an angle 2 pi (p/q i + p'/q'): powers of W.
    Value sym_trig( Frame& fr, const Rvalue& r, Trig kind )
    {
        if( r.arguments.size() != 1 || ( kind != Trig::Sin && kind != Trig::Cos ) )
        {
            sym::fail();
        }
        const Value x = operand( fr, r.arguments[0] );
        if( x.kind != Kind::Angle && x.kind != Kind::Number )
        {
            sym::fail();
        }
        const sym::Int i  = x.kind == Kind::Angle ? symbolics_[x.index].i : sym::number( 0 );
        const f64      a  = x.kind == Kind::Angle ? symbolics_[x.index].a : 0;
        const f64      b  = x.kind == Kind::Angle ? symbolics_[x.index].b : as_real( x );
        const auto     r1 = sym::rational( a / ( 2 * std::numbers::pi ) );
        const auto     r0 = sym::rational( b / ( 2 * std::numbers::pi ) );
        if( !r1 || !r0 || ( i.q != 0 && i128 { r1->first } * i.q % static_cast<i128>( r1->second ) != 0 ) )
        {
            sym::fail();
        }
        const u64 M = sym::common( r1->second, r0->second );
        // e^(i angle) = W_M^e, W_M = e^(-2 pi i / M).
        const sym::Poly e = sym::reduced(
            sym::sum(
                sym::product(
                    i.p, sym::constant( sym::narrowed( -i128 { r1->first } * static_cast<i128>( M / r1->second ) ) )
                ),
                sym::constant( sym::narrowed( -i128 { r0->first } * static_cast<i128>( M / r0->second ) ) )
            ),
            M
        );
        const sym::Poly minus = sym::reduced( sym::sum( {}, e, -1 ), M );
        // cos = (W^e + W^-e) / 2, sin = -i (W^e - W^-e) / 2.
        const i64 sign = kind == Trig::Cos ? 1 : -1;
        sym::Wave w { M, {} };
        sym::add_term( w, e, Rational( 1, 2 ) );
        sym::add_term( w, minus, Rational( sign, 2 ) );
        return make( Kind::Wave, { .w = kind == Trig::Cos ? w : sym::product( w, sym::scaled( sym::imaginary(), -1 ) ) } );
    }

    // The stream `at` is in and the element's index: a parameter's element, then only fields.
    std::pair<u32, sym::Poly> element( const Location& at )
    {
        if( at.steps.empty() || ( at.steps[0] & 3 ) == 1 )
        {
            sym::fail();
        }
        sym::Poly index =
            at.steps[0] == k_symbolic_step ? polys_[at.poly] : sym::constant( static_cast<i64>( at.steps[0] >> 2 ) );
        std::vector<u64> rest( at.steps.begin() + 1, at.steps.end() );
        if( std::ranges::any_of( rest, []( u64 s ) { return ( s & 3 ) != 1; } ) )
        {
            sym::fail();
        }
        for( u32 k = 0; k < streams_.size(); ++k )
        {
            if( streams_[k].root == at.root && streams_[k].rest == rest )
            {
                return { k, std::move( index ) };
            }
        }
        streams_.push_back( { at.root, std::move( rest ) } );
        return { static_cast<u32>( streams_.size() - 1 ), std::move( index ) };
    }

    Value stream_read( const Location& at, Type_id type )
    {
        if( at.steps.empty() )
        {
            return {}; // a by-value parameter
        }
        if( !is_f64( type ) )
        {
            sym::fail();
        }
        auto [stream, index] = element( at );
        if( streams_[stream].written )
        {
            sym::fail();
        }
        streams_[stream].read = true;
        sym::Form f;
        f.terms.emplace( sym::Key { stream, std::move( index ), 0 }, sym::wave( 1 ) );
        return terms( std::move( f ) );
    }

    void stream_write( const Location& at, Value v )
    {
        auto [stream, index] = element( at );
        if( streams_[stream].read )
        {
            sym::fail();
        }
        streams_[stream].written = true;
        sym::Form f              = as_form( v );
        if( f.carried.is_valid() )
        {
            sym::fail();
        }
        stores_.push_back( { .stream = stream, .index = std::move( index ), .value = std::move( f ), .bound = 0 } );
    }

    // Runs from `b` to a return, or within `loop` to the end of its latch's statements.
    void run_from( Frame& fr, Block_id b, u32 loop )
    {
        const Function& func = *fr.func;
        for( ;; )
        {
            spend();
            if( const u32 inner = loop_at( b ); inner != k_no_loop && inner != loop )
            {
                if( loops_.loops[inner].parent != loop )
                {
                    sym::fail();
                }
                b = run_loop( fr, inner );
                continue;
            }
            for( const Statement& s : func.statements_of( b ) )
            {
                statement( fr, s );
            }
            if( loop != k_no_loop && b == loops_.loops[loop].latch )
            {
                return;
            }
            const Terminator& t = func.blocks[b].terminator;
            span_               = t.span;
            switch( t.kind )
            {
            case Terminator_kind::Goto:
                b = t.targets[0];
                break;
            case Terminator_kind::Branch:
            {
                const Value c = operand( fr, t.condition );
                if( c.kind != Kind::Number )
                {
                    sym::fail();
                }
                b = t.targets[c.bits != 0 ? 0 : 1];
                break;
            }
            case Terminator_kind::Return:
                if( loop != k_no_loop )
                {
                    sym::fail();
                }
                return;
            default:
                sym::fail();
            }
            if( loop != k_no_loop && !loops_.loops[loop].contains( b ) )
            {
                sym::fail();
            }
        }
    }

    u32 loop_at( Block_id b ) const
    {
        for( u32 l = 0; l < loops_.loops.size(); ++l )
        {
            if( loops_.loops[l].header == b )
            {
                return l;
            }
        }
        return k_no_loop;
    }

    Value value_of( Frame& fr, Local_id x )
    {
        const auto found = memory_.find( Location { root( fr, x ), {} } );
        return found == memory_.end() ? Value {} : found->second.value;
    }

    // One turn with the index a new symbol; the locals as they are after the last turn. Returns the exit.
    Block_id run_loop( Frame& fr, u32 l )
    {
        const Function& func = *fr.func;
        const Loop&     loop = loops_.loops[l];
        if( !loop.latch.is_valid() || symbols_ == sym::k_symbols )
        {
            sym::fail();
        }
        const Terminator& test = func.blocks[loop.latch].terminator;
        if( test.kind != Terminator_kind::Branch || test.targets[0] != loop.header || loop.contains( test.targets[1] ) )
        {
            sym::fail();
        }
        const std::optional<Trip> trip = trip_count( func, types_, literals_, loop, addressed_ );
        if( !trip || trip->test != loop.latch )
        {
            sym::fail();
        }
        Local_id index {};
        for( const Induction& i : inductions_of( func, loop, addressed_ ) )
        {
            if( !types_.is_integer( func.locals[i.local].type ) )
            {
                continue;
            }
            if( index.is_valid() || i.step.kind != Operand_kind::Constant || literals_.integer( i.step.constant ) != 1 )
            {
                sym::fail();
            }
            index = i.local;
        }
        if( !index.is_valid() )
        {
            sym::fail();
        }
        const Value start = value_of( fr, index );
        if( start.kind != Kind::Number || start.bits != 0 )
        {
            sym::fail();
        }

        // Whole scalar locals only; anything else written in the loop is a parameter's element.
        std::vector<Local_id> written;
        for( const Block_id b : loop.blocks )
        {
            for( const Statement& s : func.statements_of( b ) )
            {
                if( s.kind != Statement_kind::Assign )
                {
                    continue;
                }
                if( !s.place.is_whole_local() )
                {
                    if( s.place.is_global() || s.place.local.v == 0 || s.place.local.v > func.parameter_count ||
                        !types_.is_pointer( func.locals[s.place.local].type ) )
                    {
                        sym::fail();
                    }
                }
                else if( std::ranges::find( written, s.place.local ) == written.end() )
                {
                    if( is_aggregate( func.locals[s.place.local].type ) )
                    {
                        sym::fail();
                    }
                    written.push_back( s.place.local );
                }
            }
        }

        std::vector<Value> before;
        for( const Local_id x : written )
        {
            before.push_back( value_of( fr, x ) );
            write( { root( fr, x ), {} }, is_float( func.locals[x].type ) ? terms( { .carried = x } ) : Value {} );
        }
        const u32 s      = symbols_++;
        const u64 turns  = trip->turns;
        turns_[s]        = turns;
        const Type_id it = func.locals[index].type;
        write(
            { root( fr, index ), {} }, make( Kind::Int, { .i = { sym::symbol( s ), 0, 0, static_cast<i128>( turns ) - 1 } } )
        );
        const size_t first_store = stores_.size();
        const u64    outer       = multiplicity_;
        multiplicity_            = std::min<u64>( multiplicity_ * turns, u64 { 1 } << 40 );
        run_from( fr, loop.header, l );
        multiplicity_ = outer;

        for( size_t w = 0; w < written.size(); ++w )
        {
            const Local_id x = written[w];
            const Location at { root( fr, x ), {} };
            const Value    v = value_of( fr, x );
            if( x == index )
            {
                if( v.kind != Kind::Int || symbolics_[v.index].i.q != 0 ||
                    symbolics_[v.index].i.p != sym::sum( sym::symbol( s ), sym::constant( 1 ) ) )
                {
                    sym::fail();
                }
                write( at, integer( sym::number( static_cast<i64>( turns ) ), it ) );
            }
            else if( v.kind == Kind::Terms && symbolics_[v.index].f.carried == x )
            {
                sym::Form step = symbolics_[v.index].f;
                step.carried   = {};
                sym::Form total;
                for( auto& [k, t] : step.terms )
                {
                    sym::Key bound = k;
                    bound.bound |= static_cast<u8>( 1u << s );
                    total.terms.emplace( std::move( bound ), std::move( t ) );
                }
                write( at, add( before[w], terms( std::move( total ) ), 1 ) );
            }
            else if( v.kind == Kind::Terms && symbolics_[v.index].f.carried.is_valid() )
            {
                sym::fail();
            }
            else if( depends( v, s ) )
            {
                write( at, Value {} );
            }
        }
        for( size_t i = first_store; i < stores_.size(); ++i )
        {
            stores_[i].bound |= static_cast<u8>( 1u << s );
        }
        return test.targets[1];
    }

    static std::optional<u32> one_bit( u8 mask )
    {
        return std::popcount( mask ) == 1 ? std::optional<u32>( static_cast<u32>( std::countr_zero( mask ) ) ) : std::nullopt;
    }

    // Element e of stream a, and of b unless the map is real.
    Value_path stream_path( u32 a, std::optional<u32> b, u64 e ) const
    {
        const auto one = [&]( const Stream& s )
        {
            Param_path p = roots_[s.root].path;
            p.steps.push_back( { .index = true, .element = e } );
            for( const u64 f : s.rest )
            {
                p.steps.push_back( { .field = Node_id { static_cast<u32>( f >> 2 ) } } );
            }
            return p;
        };
        return { one( streams_[a] ), b ? one( streams_[*b] ) : Param_path {} };
    }

    // y = x * K: complex from the four real forms, complex from a real x's two, or real from one.
    std::optional<Read_kernel> collect_sum() const
    {
        std::vector<u32> in, out;
        for( u32 k = 0; k < streams_.size(); ++k )
        {
            ( streams_[k].written ? out : in ).push_back( k );
        }
        const auto order = [&]( u32 a, u32 b )
        {
            return std::pair { roots_[streams_[a].root].path.local, streams_[a].rest } <
                   std::pair { roots_[streams_[b].root].path.local, streams_[b].rest };
        };
        std::ranges::sort( in, order );
        std::ranges::sort( out, order );
        const u32  parts = static_cast<u32>( out.size() );
        const bool real  = in.size() == 1;
        if( parts < 1 || parts > 2 || in.size() < 1 || in.size() > parts || stores_.size() != parts )
        {
            return std::nullopt;
        }

        const sym::Store* stored[2] {};
        for( const sym::Store& s : stores_ )
        {
            stored[s.stream == out[0] ? 0 : 1] = &s;
        }
        if( stored[0] == nullptr || ( parts == 2 && ( stored[1] == nullptr || stored[0]->bound != stored[1]->bound ) ) )
        {
            return std::nullopt;
        }
        const std::optional<u32> k = one_bit( stored[0]->bound );
        if( !k || stored[0]->index != sym::symbol( *k ) || ( parts == 2 && stored[1]->index != sym::symbol( *k ) ) )
        {
            return std::nullopt;
        }

        std::optional<u32> j;
        sym::Wave          c[2][2];
        sym::Wave          offset[2];
        for( u32 o = 0; o < parts; ++o )
        {
            for( const auto& [key, w] : stored[o]->value.terms )
            {
                if( key.stream == sym::k_one_stream )
                {
                    // b(k), in k alone.
                    for( const auto& [e, coefficient] : w.terms )
                    {
                        for( u32 s = 0; s < sym::k_symbols; ++s )
                        {
                            if( s != *k && sym::depends( e, s ) )
                            {
                                return std::nullopt;
                            }
                        }
                    }
                    if( key.bound != 0 )
                    {
                        return std::nullopt;
                    }
                    offset[o] = w;
                    continue;
                }
                const std::optional<u32> b = one_bit( key.bound );
                if( !b || *b == *k || ( j && *b != *j ) || key.index != sym::symbol( *b ) || ( real && key.stream != in[0] ) )
                {
                    return std::nullopt;
                }
                j = b;
                for( const auto& [e, coefficient] : w.terms )
                {
                    for( u32 s = 0; s < sym::k_symbols; ++s )
                    {
                        if( s != *k && s != *j && sym::depends( e, s ) )
                        {
                            return std::nullopt;
                        }
                    }
                }
                c[o][key.stream == in[0] ? 0 : 1] = w;
            }
        }
        if( !j )
        {
            return std::nullopt;
        }

        // re y = A re x - B im x and im y = B re x + A im x for a complex K = A + i B.
        if( !real && ( !sym::sum( c[0][0], c[1][1], -1 ).terms.empty() || !sym::sum( c[0][1], c[1][0] ).terms.empty() ) )
        {
            return std::nullopt;
        }
        sym::Wave K = parts == 1 ? c[0][0] : sym::sum( c[0][0], sym::product( c[1][0], sym::imaginary() ) );
        sym::Wave b = parts == 1 ? offset[0] : sym::sum( offset[0], sym::product( offset[1], sym::imaginary() ) );
        if( !b.terms.empty() )
        {
            const u64 M = sym::common( K.M, b.M );
            K           = sym::lifted( K, M );
            b           = sym::lifted( b, M );
        }
        Read_kernel r { .sum = { static_cast<u32>( turns_[*j] ), static_cast<u32>( turns_[*k] ), K.M, {}, real } };
        const auto  in_jk = [&]( const sym::Poly& e )
        {
            Exponent out;
            for( const auto& [m, v] : e )
            {
                out[{ m[*j], m[*k] }] = v;
            }
            return out;
        };
        for( const auto& [e, coefficient] : K.terms )
        {
            r.sum.coefficient[in_jk( e )] = coefficient;
        }
        for( const auto& [e, coefficient] : b.terms )
        {
            r.sum.offset[in_jk( e )] = coefficient;
        }
        r.input = {
            static_cast<u32>( std::min<u64>( adds_, ~u32 { 0 } ) ), static_cast<u32>( std::min<u64>( muls_, ~u32 { 0 } ) )
        };
        for( u64 x = 0; x < r.sum.n; ++x )
        {
            r.inputs.push_back( stream_path( in[0], real ? std::nullopt : std::optional( in[1] ), x ) );
        }
        for( u64 y = 0; y < r.sum.m; ++y )
        {
            r.outputs.push_back( stream_path( out[0], parts == 2 ? std::optional( out[1] ) : std::nullopt, y ) );
        }
        return r;
    }

    // The return slot is an output; a pointer parameter's referent is an input and an output, and any
    // other parameter an input.
    void set_up( Frame& top )
    {
        const Function& func = *top.func;
        const u32       r    = root( top, k_return_slot );
        roots_[r].output     = func.returns_a_value;
        roots_[r].path       = { k_return_slot, false, {} };
        for( u32 p = 1; p <= func.parameter_count; ++p )
        {
            const Local_id local { p };
            const u32      own = root( top, local );
            if( types_.is_pointer( func.locals[local].type ) )
            {
                const u32 referent = static_cast<u32>( roots_.size() );
                roots_.push_back( { true, true, { local, true, {} } } );
                write( { own, {} }, { .kind = Kind::Address, .index = address_of( { referent, {} } ) } );
            }
            else
            {
                roots_[own] = { true, false, { local, false, {} } };
            }
        }
    }

    Param_path path( const Location& at ) const
    {
        Param_path a = roots_[at.root].path;
        for( const u64 step : at.steps )
        {
            a.steps.push_back(
                ( step & 3 ) == 1 ? Step { .field = Node_id { static_cast<u32>( step >> 2 ) } }
                                  : Step { .index = true, .element = step >> 2 }
            );
        }
        return a;
    }

    using Pairs = std::vector<std::pair<u32, u32>>;

    static std::vector<Pairs> pairings( u32 n )
    {
        std::vector<Pairs> out;
        if( n % 2 != 0 || n == 0 )
        {
            return out;
        }
        Pairs halves, adjacent;
        for( u32 i = 0; i < n / 2; ++i )
        {
            halves.emplace_back( i, i + n / 2 );
            adjacent.emplace_back( 2 * i, 2 * i + 1 );
        }
        out.push_back( halves );
        out.push_back( adjacent );
        return out;
    }

    std::variant<Recognised, Unrecognised> collect()
    {
        span_ = {};
        std::vector<Location> outs;
        std::vector<Form>     rows_of;
        for( const auto& [at, cell] : memory_ )
        {
            if( !roots_[at.root].output )
            {
                continue;
            }
            if( cell.value.kind == Kind::Form )
            {
                rows_of.push_back( forms_[cell.value.index] );
            }
            else if( cell.value.kind == Kind::Number && ( !ring_ || is_zero( cell.value ) ) )
            {
                rows_of.emplace_back();
            }
            else if( cell.value.kind == Kind::Number )
            {
                span_ = cell.span;
                rows_of.push_back( forms_[constant_form( cell.value ).index] );
            }
            else
            {
                end( Ending::Unknown, cell.span );
            }
            outs.push_back( at );
        }
        span_ = {};
        if( outs.empty() || inputs_.size() == ( one_ == ~0u ? 0u : 1u ) )
        {
            end( Ending::Empty );
        }

        // Columns in place order.
        std::vector<u32> order( inputs_.size() );
        std::iota( order.begin(), order.end(), 0u );
        std::ranges::sort( order, [&]( u32 a, u32 b ) { return inputs_[a] < inputs_[b]; } );
        std::vector<u32> column( inputs_.size() );
        for( u32 j = 0; j < order.size(); ++j )
        {
            column[order[j]] = j;
        }

        // Every coefficient as an id comparable with the others: bits, or an element of Q(W_m).
        u32 m = 1;
        for( const Form& f : rows_of )
        {
            for( const Term& t : f )
            {
                m = integer_ ? 1 : common( m, t.exact.m );
            }
        }
        const u32                     cols = static_cast<u32>( inputs_.size() );
        std::vector<std::vector<u64>> ids( outs.size(), std::vector<u64>( cols, integer_ ? 0 : field( m ).zero().v ) );
        for( size_t i = 0; i < rows_of.size(); ++i )
        {
            for( const Term& t : rows_of[i] )
            {
                ids[i][column[t.input]] = integer_ ? t.bits : lift( t.exact, m ).v;
            }
        }
        const auto negate = [&]( u64 id )
        { return integer_ ? wrap( 0 - id ) : field( m ).neg( Element { static_cast<u32>( id ) } ).v; };

        std::optional<std::pair<Pairs, Pairs>> paired;
        // The input fixed at 1 sorts last and pairs with none: its coefficients make one complex constant.
        const bool affine = one_ != ~0u;
        for( const Pairs& rp : pairings( static_cast<u32>( outs.size() ) ) )
        {
            for( const Pairs& cp : pairings( cols - affine ) )
            {
                bool rotation = true;
                for( const auto& [p, q] : rp )
                {
                    for( const auto& [u, v] : cp )
                    {
                        rotation = rotation && ids[p][u] == ids[q][v] && ids[p][v] == negate( ids[q][u] );
                    }
                }
                if( rotation && !paired )
                {
                    paired = { rp, cp };
                }
            }
        }

        const Ring_kind kind = integer_ ? Ring_kind::Integer : paired ? Ring_kind::Complex : Ring_kind::Real;
        const u32       n    = integer_ ? ( paired ? 4 : 1 ) : paired ? std::lcm( m, 4u ) : m;
        Recognised      out {
                 .field   = Field( kind, n, integer_ ? width_ : 64 ),
                 .rows    = {},
                 .inputs  = {},
                 .outputs = {},
                 .type    = type_,
                 .input   = cost_
        };
        Field&     f       = out.field;
        const auto element = [&]( u64 id ) {
            return integer_ ? f.rational( as_signed( id, width_ ) )
                            : lift_into( field( m ), Element { static_cast<u32>( id ) }, f );
        };
        std::vector<Location> ins( cols );
        for( u32 j = 0; j < cols; ++j )
        {
            ins[column[j]] = inputs_[j];
        }

        if( !paired )
        {
            for( size_t i = 0; i < outs.size(); ++i )
            {
                Row row;
                for( u32 j = 0; j < cols; ++j )
                {
                    row.push_back( element( ids[i][j] ) );
                }
                out.rows.push_back( std::move( row ) );
                out.outputs.push_back( { path( outs[i] ), {} } );
            }
            for( const Location& at : ins )
            {
                out.inputs.push_back( at.root == k_one_root ? Value_path {} : Value_path { path( at ), {} } );
            }
            return out;
        }

        // C + S i, where i is W^(3n/4): W_4 is -i.
        const Element i = f.root( 3 * n / 4 );
        for( const auto& [p, q] : paired->first )
        {
            Row row;
            for( const auto& [u, v] : paired->second )
            {
                row.push_back( f.add( element( ids[p][u] ), f.mul( element( ids[q][u] ), i ) ) );
            }
            if( affine )
            {
                row.push_back( f.add( element( ids[p][cols - 1] ), f.mul( element( ids[q][cols - 1] ), i ) ) );
            }
            out.rows.push_back( std::move( row ) );
            out.outputs.push_back( { path( outs[p] ), path( outs[q] ) } );
        }
        for( const auto& [u, v] : paired->second )
        {
            out.inputs.push_back( { path( ins[u] ), path( ins[v] ) } );
        }
        if( affine )
        {
            out.inputs.emplace_back();
        }
        return out;
    }

    const std::vector<Function>& functions_;
    const Purity&                purity_;
    const Type_table&            types_;
    const Literal_pool&          literals_;
    u64                          budget_;
    const Global_constants&      globals_;
    u64                          steps_ = 0;
    u32                          depth_ = 0;
    Span                         span_ {};

    std::map<u32, Field>                             fields_;
    std::unordered_map<u64, Element>                 lifts_;
    std::unordered_map<u64, std::optional<Exact>>    literal_exacts_;
    std::vector<Root>                                roots_;
    std::unordered_map<u32, std::pair<u32, Type_id>> global_roots_; // by declaration
    std::unordered_map<u32, u32>                     table_roots_;  // a constant array's, by its literal
    std::vector<Location>                            locations_;
    std::map<Location, Cell>                         memory_; // what the run wrote
    std::map<Location, Value>                        read_;   // inputs not yet overwritten
    std::vector<Form>                                forms_;
    std::vector<Location>                            inputs_;        // in the order first read
    u32                                              one_     = ~0u; // the input fixed at 1, once a constant is added
    bool                                             ring_    = false;
    bool                                             integer_ = false;
    u8                                               width_   = 64;
    Type_id                                          type_ {};
    Cost                                             cost_ {};

    // Symbolic mode.
    bool                            symbolic_ = false;
    Loops                           loops_ {};
    Local_set                       addressed_ {};
    std::vector<Symbolic>           symbolics_;
    std::vector<sym::Poly>          polys_ { {} }; // 0 is none
    std::vector<Stream>             streams_;
    std::vector<sym::Store>         stores_;
    std::array<u64, sym::k_symbols> turns_ {};
    u32                             symbols_      = 0;
    u64                             multiplicity_ = 1;
    u64                             adds_         = 0;
    u64                             muls_         = 0;
};
} // namespace

std::string_view describe( Ending why )
{
    switch( why )
    {
    case Ending::Product:
        return "multiplies two inputs";
    case Ending::Compare:
        return "compares an input";
    case Ending::Branch:
        return "branches or indexes on a value known only at run time";
    case Ending::Escape:
        return "passes an input somewhere other than an output";
    case Ending::Effect:
        return "has an effect other than its outputs";
    case Ending::Shift:
        return "shifts an integer input";
    case Ending::Division:
        return "divides by an input, or divides an integer input";
    case Ending::Remainder:
        return "takes an integer input's remainder";
    case Ending::Bits:
        return "reads an input's bits";
    case Ending::Cast:
        return "casts an input to another type";
    case Ending::Coefficient:
        return "scales an input by a value with no exact form";
    case Ending::Unknown:
        return "computes an output from a value known only at run time";
    case Ending::Global:
        return "reads a global";
    case Ending::Panic:
        return "can panic";
    case Ending::Unsupported:
        return "does something recognition cannot follow";
    case Ending::Mixed:
        return "reads inputs of two types";
    case Ending::Empty:
        return "computes nothing from its inputs";
    case Ending::Budget:
        return "runs for too long";
    }
    return "";
}

std::optional<Read_kernel> read_kernel(
    const Function&              func,
    const std::vector<Function>& functions,
    const Purity&                purity,
    const Type_table&            types,
    const Literal_pool&          literals,
    const Global_constants&      globals
)
{
    return Recogniser( functions, purity, types, literals, 4'000'000, globals ).read_sum( func );
}

std::variant<Recognised, Unrecognised> recognise(
    const Function&              func,
    const std::vector<Function>& functions,
    const Purity&                purity,
    const Type_table&            types,
    const Literal_pool&          literals,
    const Global_constants&      globals,
    u64                          budget
)
{
    return Recogniser( functions, purity, types, literals, budget, globals ).run( func );
}
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include "check/pipeline_test_support.h"
#include "ir/call_graph.h"
#include "ir/print.h"

namespace keel
{
namespace
{
constexpr std::string_view k_trig = "@trig_cos f64 cosine( f64 x ) { return x; }\n"
                                    "@trig_sin f64 sine( f64 x ) { return x; }\n";

// At -O3 without the loop phase, where the pass runs; the function under test is the first.
struct Attempt : Compiled
{
    std::variant<Recognised, Unrecognised> result;

    explicit Attempt( std::string_view source, u64 budget = 4'000'000 )
        : Compiled(
              std::string( source ) + std::string( k_trig ) + "i32 main() { return 0; }\n",
              Stage::Elaborated,
              Build { .level = 3, .loops = std::nullopt }
          ),
          result( Unrecognised { Ending::Empty, {} } )
    {
        INFO( rendered() );
        REQUIRE( clean() );
        Purity purity( all, ast );
        for( const std::vector<u32>& group : callees_first( all ) )
        {
            purity.decide( group );
        }
        const Global_constants globals = [&]( Node_id decl ) -> std::optional<Global_constant>
        {
            const std::optional<Constant_value> value = types.constant_of( decl );
            if( !value || !ast.is_const_binding( decl ) )
            {
                return std::nullopt;
            }
            return Global_constant { *value, types.type_of( decl ) };
        };
        result = recognise( functions.front(), all, purity, types.table(), literals, globals, budget );
    }

    std::string kir() const
    {
        return print( functions.front(), ast, types.table(), literals, interner );
    }

    const Recognised& map() const
    {
        INFO( kir() );
        if( const Unrecognised* u = std::get_if<Unrecognised>( &result ) )
        {
            FAIL( describe( u->why ) );
        }
        return std::get<Recognised>( result );
    }

    Ending ending() const
    {
        INFO( kir() );
        REQUIRE( std::holds_alternative<Unrecognised>( result ) );
        return std::get<Unrecognised>( result ).why;
    }
};

bool is_dft4( const Recognised& r )
{
    if( r.field.kind() != Ring_kind::Complex || r.rows.size() != 4 || r.inputs.size() != 4 )
    {
        return false;
    }
    for( u32 k = 0; k < 4; ++k )
    {
        for( u32 j = 0; j < 4; ++j )
        {
            if( std::abs( r.field.value( r.rows[k][j] ) - std::polar( 1.0, -2 * std::numbers::pi * j * k / 4 ) ) > 1e-12 )
            {
                return false;
            }
        }
    }
    return true;
}
// As Attempt, read as a kernel.
std::optional<Read_kernel> read_first( std::string_view source )
{
    const Compiled c(
        std::string( source ) + std::string( k_trig ) + "i32 main() { return 0; }\n",
        Stage::Elaborated,
        Build { .level = 3, .loops = std::nullopt }
    );
    INFO( c.rendered() );
    REQUIRE( c.clean() );
    Purity purity( c.all, c.ast );
    for( const std::vector<u32>& group : callees_first( c.all ) )
    {
        purity.decide( group );
    }
    return read_kernel( c.functions.front(), c.all, purity, c.types.table(), c.literals );
}

std::string dft( std::string_view angle, std::string_view body = "sr += re[j] * c - im[j] * s;\n si += re[j] * s + im[j] * c;" )
{
    return "@real_arithmetic void f( const ref f64[96] re, const ref f64[96] im, ref f64[96] yr, ref f64[96] yi ) {\n"
           "for( u64 k = 0; k < 96; k += 1 ) { f64 sr = 0.0; f64 si = 0.0;\n"
           "for( u64 j = 0; j < 96; j += 1 ) { const f64 t = " +
           std::string( angle ) + ";\n const f64 c = cosine( t ); const f64 s = sine( t );\n" + std::string( body ) +
           " }\n yr[k] = sr; yi[k] = si; } }\n";
}
} // namespace

TEST_CASE( "recognise_pairs_a_dft4_loop", "[linear][recognise]" )
{
    const Attempt     a( "@real_arithmetic\n"
                         "void dft( const ref f64[4] re, const ref f64[4] im, ref f64[4] yr, ref f64[4] yi )\n"
                         "{\n"
                         "    for( u64 k = 0; k < 4; k += 1 )\n"
                         "    {\n"
                         "        f64 sr = 0.0;\n"
                         "        f64 si = 0.0;\n"
                         "        for( u64 j = 0; j < 4; j += 1 )\n"
                         "        {\n"
                         "            const f64 t = -2.0 * 3.141592653589793 * cast<f64>( j * k ) / 4.0;\n"
                         "            const f64 c = cosine( t );\n"
                         "            const f64 s = sine( t );\n"
                         "            sr += re[j] * c - im[j] * s;\n"
                         "            si += re[j] * s + im[j] * c;\n"
                         "        }\n"
                         "        yr[k] = sr;\n"
                         "        yi[k] = si;\n"
                         "    }\n"
                         "}\n" );
    const Recognised& r = a.map();
    CHECK( is_dft4( r ) );
    CHECK( r.field.n() == 4 );
    CHECK( r.input == Cost { .adds = 64, .muls = 64 } );
    CHECK( r.inputs[1].real.local == Local_id { 1 } );
    CHECK( r.inputs[1].real.through );
    CHECK( r.inputs[1].real.steps.size() == 1 );
    CHECK( r.inputs[1].real.steps[0].element == 1 );
    CHECK( r.inputs[1].imaginary.local == Local_id { 2 } );
    CHECK( r.outputs[3].real.local == Local_id { 3 } );
    CHECK( r.outputs[3].imaginary.local == Local_id { 4 } );
}

TEST_CASE( "recognise_pairs_a_dft4_by_hand", "[linear][recognise]" )
{
    const Attempt a( "@real_arithmetic\n"
                     "void dft( const ref f64[4] r, const ref f64[4] i, ref f64[4] yr, ref f64[4] yi )\n"
                     "{\n"
                     "    yr[0] = r[0] + r[1] + r[2] + r[3];\n"
                     "    yi[0] = i[0] + i[1] + i[2] + i[3];\n"
                     "    yr[1] = r[0] + i[1] - r[2] - i[3];\n"
                     "    yi[1] = i[0] - r[1] - i[2] + r[3];\n"
                     "    yr[2] = r[0] - r[1] + r[2] - r[3];\n"
                     "    yi[2] = i[0] - i[1] + i[2] - i[3];\n"
                     "    yr[3] = r[0] - i[1] - r[2] + i[3];\n"
                     "    yi[3] = i[0] + r[1] - i[2] - r[3];\n"
                     "}\n" );
    CHECK( is_dft4( a.map() ) );
    CHECK( a.map().input == Cost { .adds = 24, .muls = 0 } );
}

TEST_CASE( "recognise_reads_an_integer_wht4_in_place", "[linear][recognise]" )
{
    const Attempt     a( "@real_arithmetic\n"
                         "void wht( ref i32[4] x )\n"
                         "{\n"
                         "    const i32 a = x[0] + x[1];\n"
                         "    const i32 b = x[0] - x[1];\n"
                         "    const i32 c = x[2] + x[3];\n"
                         "    const i32 d = x[2] - x[3];\n"
                         "    x[0] = a + c;\n"
                         "    x[1] = b + d;\n"
                         "    x[2] = a - c;\n"
                         "    x[3] = b - d;\n"
                         "}\n" );
    const Recognised& r = a.map();
    REQUIRE( r.field.kind() == Ring_kind::Integer );
    CHECK( r.field.n() == 1 );
    CHECK( r.field.width() == 32 );
    const Element    p = r.field.one();
    const Element    q = r.field.minus_one();
    std::vector<Row> wht { { p, p, p, p }, { p, q, p, q }, { p, p, q, q }, { p, q, q, p } };
    CHECK( r.rows == wht );
    CHECK( r.input == Cost { .adds = 8, .muls = 0 } );
}

TEST_CASE( "recognise_reads_cos_constants_and_roots_exactly", "[linear][recognise]" )
{
    // cos(pi/4) as a literal and as a root, and sqrt 3 = 2 cos(pi/6).
    const Attempt     a( "@real_arithmetic\n"
                         "void f( const ref f64[3] x, ref f64[3] y )\n"
                         "{\n"
                         "    y[0] = x[0] * 0.7071067811865476;\n"
                         "    y[1] = x[1] * sqrt_f64( 0.5 );\n"
                         "    y[2] = x[2] * sqrt_f64( 3.0 );\n"
                         "}\n" );
    const Recognised& r = a.map();
    CHECK( r.field.n() == 24 );
    CHECK( r.rows[0][0] == r.rows[1][1] );
    CHECK( std::abs( r.field.value( r.rows[0][0] ) - std::sqrt( 0.5 ) ) < 1e-15 );
    CHECK( std::abs( r.field.value( r.rows[2][2] ) - std::sqrt( 3.0 ) ) < 1e-15 );
}

// 0.1 is 1/10, so 0.2 is twice it and 0.3 their sum; 1.0 / 3.0 folds to 1/3.
TEST_CASE( "recognise_reads_a_decimal_as_its_fraction", "[linear][recognise]" )
{
    const Attempt a( "@real_arithmetic\n"
                     "void f( const ref f64[2] x, ref f64[2] y )\n"
                     "{\n"
                     "    y[0] = 0.1 * x[0] + 0.2 * x[1];\n"
                     "    y[1] = 0.3 * x[0] + x[1] / 3.0;\n"
                     "}\n" );
    Recognised    r = a.map();
    CHECK( r.rows[0][1] == r.field.add( r.rows[0][0], r.rows[0][0] ) );
    CHECK( r.rows[1][0] == r.field.add( r.rows[0][0], r.rows[0][1] ) );
    CHECK( r.field.mul( r.rows[1][1], r.field.rational( 3 ) ) == r.field.rational( 1 ) );
}

TEST_CASE( "recognise_reads_a_const_global_table", "[linear][recognise]" )
{
    const Attempt     a( "@real_arithmetic\n"
                         "void f( const ref f64[3] x, ref f64[3] y )\n"
                         "{\n"
                         "    for( u64 i = 0; i < 3; i += 1 ) { y[i] = x[i] * q[i]; }\n"
                         "}\n"
                         "const f64[3] q = [ 0.5, 3.0, 0.75 ];\n" );
    const Recognised& r = a.map();
    CHECK( r.field.value( r.rows[0][0] ) == 0.5 );
    CHECK( r.field.value( r.rows[1][1] ) == 3.0 );
    CHECK( r.field.value( r.rows[2][2] ) == 0.75 );
}

TEST_CASE( "recognise_reads_a_constant_as_the_input_fixed_at_1", "[linear][recognise]" )
{
    const Attempt     a( "@real_arithmetic\n"
                         "void f( const ref f64[2] x, ref f64[3] y )\n"
                         "{\n"
                         "    y[0] = 2.0 * ( x[0] + 1.5 );\n"
                         "    y[1] = 0.5 - x[1];\n"
                         "    y[2] = 4.0;\n"
                         "}\n" );
    const Recognised& r = a.map();
    REQUIRE( r.inputs.size() == 3 );
    CHECK( r.inputs[0].real.local.is_valid() );
    CHECK( !r.inputs[2].real.local.is_valid() );
    const auto at = [&]( u32 k, u32 j ) { return r.field.value( r.rows[k][j] ).real(); };
    CHECK( at( 0, 0 ) == 2.0 );
    CHECK( at( 0, 2 ) == 3.0 );
    CHECK( at( 1, 1 ) == -1.0 );
    CHECK( at( 1, 2 ) == 0.5 );
    CHECK( at( 2, 2 ) == 4.0 );
}

TEST_CASE( "recognise_ends_with_the_operation_that_stops_it", "[linear][recognise]" )
{
    const auto ending = []( std::string_view body, u64 budget = 4'000'000 )
    { return Attempt( "@real_arithmetic\n" + std::string( body ), budget ).ending(); };

    CHECK( ending( "f64 f( const ref f64[2] x ) { return x[0] * x[1]; }\n" ) == Ending::Product );
    CHECK( ending( "f64 f( f64 x ) { if( x < 0.0 ) { return 0.0; } return x; }\n" ) == Ending::Compare );
    CHECK( ending( "f64 f( f64 x, bool flip ) { if( flip ) { return x; } return 2.0 * x; }\n" ) == Ending::Branch );
    CHECK( ending( "f64 f( f64 x ) { g = x; return x; }\nf64 g;\n" ) == Ending::Escape );
    CHECK( ending( "i64 f( i64 x ) { return x << 1; }\n" ) == Ending::Shift );
    CHECK( ending( "i64 f( i64 x ) { return x / 3; }\n" ) == Ending::Division );
    CHECK( ending( "i64 f( i64 x ) { return x % 3; }\n" ) == Ending::Remainder );
    CHECK( ending( "i64 f( i32 x ) { return cast<i64>( x ); }\n" ) == Ending::Cast );
    CHECK( ending( "f64 f( f64 x ) { return x * 0.123456789; }\n" ) == Ending::Coefficient );
    CHECK( ending( "f64 f( f64 x ) { return x * g; }\nf64 g = 0.5;\n" ) == Ending::Global );
    CHECK(
        ending( "f64 f( f64 x ) { f64 s = 0.0; for( u64 i = 0; i < 1000; i += 1 ) { s += x; } return s; }\n", 100 ) ==
        Ending::Budget
    );
}

TEST_CASE( "a DFT loop nest reads as its kernel", "[linear][recognise]" )
{
    const std::optional<Read_kernel> r = read_first( dft( "-2.0 * 3.141592653589793 * cast<f64>( j * k % 96 ) / 96.0" ) );
    REQUIRE( r );
    CHECK( r->sum == exponent_kernel( 96, 96, 96, 1 ) );
    CHECK( r->inputs.size() == 96 );
    CHECK( r->outputs.size() == 96 );
    CHECK( r->input.muls == 4 * 96 * 96 );
}

TEST_CASE( "a DFT loop of real x reads as a real sum", "[linear][recognise]" )
{
    const std::optional<Read_kernel> r =
        read_first( dft( "-2.0 * 3.141592653589793 * cast<f64>( j * k % 96 ) / 96.0", "sr += re[j] * c;\n si += re[j] * s;" ) );
    REQUIRE( r );
    Sum want  = exponent_kernel( 96, 96, 96, 1 );
    want.real = true;
    CHECK( r->sum == want );
    CHECK( !r->inputs[0].imaginary.local.is_valid() );
    CHECK( r->outputs[0].imaginary.local.is_valid() );
}

TEST_CASE( "a DFT loop's starting values read as its offset", "[linear][recognise]" )
{
    std::string source = dft( "-2.0 * 3.141592653589793 * cast<f64>( j * k ) / 96.0" );
    const auto  at     = source.find( "f64 sr = 0.0; f64 si = 0.0;" );
    REQUIRE( at != std::string::npos );
    source.replace( at, 27, "f64 sr = 1.0; f64 si = cosine( 2.0 * 3.141592653589793 * cast<f64>( k ) / 96.0 );" );
    const std::optional<Read_kernel> r = read_first( source );
    REQUIRE( r );
    // 1 + i cos(2 pi k / 96), i = -W^24.
    Sum want    = exponent_kernel( 96, 96, 96, 1 );
    want.offset = {
        { Exponent {}, 1 },
        { Exponent { { { 0, 1 }, 1 }, { { 0, 0 }, 24 } }, Rational( -1, 2 ) },
        { Exponent { { { 0, 1 }, 95 }, { { 0, 0 }, 24 } }, Rational( -1, 2 ) },
    };
    CHECK( r->sum == want );
}

TEST_CASE( "an inverse DFT reads as c = M - 1, without the remainder too", "[linear][recognise]" )
{
    const std::optional<Read_kernel> r = read_first( dft( "2.0 * 3.141592653589793 * cast<f64>( j * k ) / 96.0" ) );
    REQUIRE( r );
    CHECK( r->sum == exponent_kernel( 96, 96, 96, 95 ) );
}

TEST_CASE( "a loop that is not one complex kernel reads as none", "[linear][recognise]" )
{
    // Two inputs multiplied.
    CHECK( !read_first( dft( "-2.0 * 3.141592653589793 * cast<f64>( j * k ) / 96.0", "sr += re[j] * im[j]; si += im[j] * c;" ) )
    );
    // Not a rotation: cos on both parts.
    CHECK( !read_first( dft( "-2.0 * 3.141592653589793 * cast<f64>( j * k ) / 96.0", "sr += re[j] * c; si += im[j] * s;" ) ) );
}

TEST_CASE( "a real DCT loop reads as a real sum", "[linear][recognise]" )
{
    const std::optional<Read_kernel> r = read_first(
        "@real_arithmetic void f( const ref f64[96] x, ref f64[96] y ) {\n"
        "for( u64 k = 0; k < 96; k += 1 ) { f64 s = 0.0;\n"
        "for( u64 n = 0; n < 96; n += 1 ) { s += x[n] * cosine( 3.141592653589793 / 96.0 * ( cast<f64>( n ) + 0.5 ) * "
        "cast<f64>( k ) ); }\n y[k] = s; } }\n"
    );
    REQUIRE( r );
    const Sum dct {
        96,
        96,
        384,
        { { Exponent { { { 1, 1 }, 2 }, { { 0, 1 }, 1 } }, Rational( 1, 2 ) },
          { Exponent { { { 1, 1 }, 382 }, { { 0, 1 }, 383 } }, Rational( 1, 2 ) } },
        true
    };
    CHECK( r->sum == dct );
    CHECK( !r->inputs[0].imaginary.local.is_valid() );
}

TEST_CASE( "an angle quadratic in j reads as its exponent", "[linear][recognise]" )
{
    const std::optional<Read_kernel> r = read_first( dft( "-2.0 * 3.141592653589793 * cast<f64>( j * j * k ) / 96.0" ) );
    REQUIRE( r );
    CHECK( r->sum == Sum { 96, 96, 96, { { Exponent { { { 2, 1 }, 1 } }, 1 } } } );
}

TEST_CASE( "a kernel's coefficients are exact", "[linear][recognise]" )
{
    const std::optional<Read_kernel> r = read_first(
        dft( "-2.0 * 3.141592653589793 * cast<f64>( j * k ) / 96.0",
             "sr += ( re[j] * c - im[j] * s ) / 3.0;\n si += ( re[j] * s + im[j] * c ) / 3.0;" )
    );
    REQUIRE( r );
    Sum third                         = exponent_kernel( 96, 96, 96, 1 );
    third.coefficient.begin()->second = Rational( 1, 3 );
    CHECK( r->sum == third );
}

TEST_CASE( "a kernel's pure calls run as the rest of recognition runs them", "[linear][recognise]" )
{
    const std::optional<Read_kernel> r = read_first(
        dft( "-2.0 * 3.141592653589793 * cast<f64>( j * k ) / 96.0",
             "sr += rot( re[j], im[j], c, s ); si += rot( im[j], re[j], c, -s );" ) +
        "f64 rot( f64 a, f64 b, f64 c, f64 s ) { return a * c - b * s; }\n"
    );
    REQUIRE( r );
    CHECK( r->sum == exponent_kernel( 96, 96, 96, 1 ) );
}
} // namespace keel
#endif // ENABLE_UNIT_TESTS
