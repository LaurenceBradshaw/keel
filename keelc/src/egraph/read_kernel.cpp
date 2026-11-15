// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "egraph/read_kernel.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include "ir/loops.h"
#include "opt/fold_constants.h"
#include "opt/forward_reads.h"
#include "opt/reduce_inductions.h"

namespace keel
{
namespace
{
using i128 = __int128;

constexpr u32 k_symbols      = 8;
constexpr u64 k_largest_root = u64 { 1 } << 24; // the largest M an angle is matched at
constexpr u64 k_steps        = 1'000'000;       // blocks run, each loop's body once
constexpr f64 k_zero         = 1e-12;           // a coefficient this small is gone

struct Failed
{
};

[[noreturn]] void fail()
{
    throw Failed {};
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

// sum over Poly e of c W_M^e: a function of the symbols.
struct Wave
{
    u64                               M = 1;
    std::map<Poly, std::complex<f64>> terms;
};

void add_term( Wave& w, const Poly& e, std::complex<f64> c )
{
    std::complex<f64>& t = w.terms[e];
    t += c;
    if( std::abs( t ) < k_zero )
    {
        w.terms.erase( e );
    }
}

Wave wave( std::complex<f64> c )
{
    Wave w;
    add_term( w, {}, c );
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

Wave sum( const Wave& a, const Wave& b, std::complex<f64> sign = 1 )
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

Wave scaled( const Wave& a, std::complex<f64> c )
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

// A linear form over input elements; `carried` is a local's value at the turn's start, added once.
struct Form
{
    std::map<Key, Wave> terms {};
    Local_id            carried {};
};

Form sum( const Form& a, const Form& b, f64 sign )
{
    if( b.carried.is_valid() && ( sign < 0 || a.carried.is_valid() ) )
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

enum class Kind : u8
{
    Unknown,
    Int,
    Real,  // b
    Angle, // a i + b
    Wave,
    Form,
};

struct Value
{
    Kind kind = Kind::Unknown;
    Int  i {};
    f64  a = 0;
    f64  b = 0;
    Wave w {};
    Form f {};
};

Value of( Int i )
{
    return { .kind = Kind::Int, .i = std::move( i ) };
}

Value real( f64 x )
{
    return { .kind = Kind::Real, .b = x };
}

Value of( Wave w )
{
    return { .kind = Kind::Wave, .w = std::move( w ) };
}

Value of( Form f )
{
    return { .kind = Kind::Form, .f = std::move( f ) };
}

bool depends( const Value& v, u32 s )
{
    switch( v.kind )
    {
    case Kind::Int:
    case Kind::Angle:
        return depends( v.i.p, s );
    case Kind::Wave:
        return depends( v.w, s );
    case Kind::Form:
        return std::ranges::any_of(
            v.f.terms,
            [s]( const auto& t )
            { return ( ( t.first.bound >> s & 1 ) == 0 && depends( t.first.index, s ) ) || depends( t.second, s ); }
        );
    case Kind::Unknown:
    case Kind::Real:
        return false;
    }
    return false;
}

// What a parameter's elements are read or written through: `local`, deref'd when `through`, indexed, then `fields`.
struct Stream
{
    Local_id             local;
    bool                 through = false;
    std::vector<Node_id> fields {};
    bool                 read    = false;
    bool                 written = false;
};

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

class Reader
{
public:
    Reader(
        const Function&              func,
        const std::vector<Function>& functions,
        const Purity&                purity,
        const Type_table&            types,
        const Literal_pool&          literals
    )
        : func_( func ),
          functions_( functions ),
          purity_( purity ),
          types_( types ),
          literals_( literals ),
          loops_( find_loops( func ) ),
          addressed_( addressed_locals_of( func ) ),
          locals_( func.locals.size() )
    {
    }

    std::optional<Read_kernel> run()
    {
        try
        {
            if( func_.returns_a_value )
            {
                return std::nullopt;
            }
            run_from( k_entry_block, k_no_loop );
            return collect();
        }
        catch( const Failed& )
        {
            return std::nullopt;
        }
        catch( const Field::Overflow& )
        {
            return std::nullopt;
        }
    }

private:
    // Runs from `b` to a return, or within `loop` to the end of its latch's statements.
    void run_from( Block_id b, u32 loop )
    {
        for( ;; )
        {
            if( ++steps_ > k_steps )
            {
                fail();
            }
            if( const u32 inner = loop_at( b ); inner != k_no_loop && inner != loop )
            {
                if( loops_.loops[inner].parent != loop )
                {
                    fail();
                }
                b = run_loop( inner );
                continue;
            }
            for( const Statement& s : func_.statements_of( b ) )
            {
                execute( s );
            }
            if( loop != k_no_loop && b == loops_.loops[loop].latch )
            {
                return;
            }
            const Terminator& t = func_.blocks[b].terminator;
            switch( t.kind )
            {
            case Terminator_kind::Goto:
                b = t.targets[0];
                break;
            case Terminator_kind::Branch:
            {
                const Value c = operand( t.condition );
                if( c.kind != Kind::Int || !concrete( c.i ) )
                {
                    fail();
                }
                b = t.targets[*constant_of( c.i.p ) != 0 ? 0 : 1];
                break;
            }
            case Terminator_kind::Return:
                if( loop != k_no_loop )
                {
                    fail();
                }
                return;
            default:
                fail();
            }
            if( loop != k_no_loop && !loops_.loops[loop].contains( b ) )
            {
                fail();
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

    // One turn with the index a new symbol; the locals as they are after the last turn. Returns the exit.
    Block_id run_loop( u32 l )
    {
        const Loop& loop = loops_.loops[l];
        if( !loop.latch.is_valid() || symbols_ == k_symbols )
        {
            fail();
        }
        const Terminator& test = func_.blocks[loop.latch].terminator;
        if( test.kind != Terminator_kind::Branch || test.targets[0] != loop.header || loop.contains( test.targets[1] ) )
        {
            fail();
        }
        const std::optional<Trip> trip = trip_count( func_, types_, literals_, loop, addressed_ );
        if( !trip || trip->test != loop.latch )
        {
            fail();
        }
        Local_id index {};
        for( const Induction& i : inductions_of( func_, loop, addressed_ ) )
        {
            if( !types_.is_integer( func_.locals[i.local].type ) )
            {
                continue;
            }
            if( index.is_valid() || i.step.kind != Operand_kind::Constant || literals_.integer( i.step.constant ) != 1 )
            {
                fail();
            }
            index = i.local;
        }
        if( !index.is_valid() || locals_[index.v].kind != Kind::Int || !concrete( locals_[index.v].i ) ||
            *constant_of( locals_[index.v].i.p ) != 0 )
        {
            fail();
        }

        std::vector<Local_id> written;
        for( const Block_id b : loop.blocks )
        {
            for( const Statement& s : func_.statements_of( b ) )
            {
                if( s.kind == Statement_kind::Assign && s.place.is_whole_local() &&
                    std::ranges::find( written, s.place.local ) == written.end() )
                {
                    written.push_back( s.place.local );
                }
            }
        }

        const std::vector<Value> before = locals_;
        for( const Local_id x : written )
        {
            locals_[x.v] = types_.is_float( func_.locals[x].type ) ? of( Form { .carried = x } ) : Value {};
        }
        const u32 s              = symbols_++;
        const u64 turns          = trip->turns;
        turns_[s]                = turns;
        locals_[index.v]         = of( Int { symbol( s ), 0, 0, static_cast<i128>( turns ) - 1 } );
        const size_t first_store = stores_.size();
        const u64    outer       = multiplicity_;
        multiplicity_            = std::min<u64>( multiplicity_ * turns, u64 { 1 } << 40 );
        run_from( loop.header, l );
        multiplicity_ = outer;

        for( const Local_id x : written )
        {
            Value& v = locals_[x.v];
            if( x == index )
            {
                if( v.kind != Kind::Int || v.i.q != 0 || v.i.p != sum( symbol( s ), constant( 1 ) ) )
                {
                    fail();
                }
                v = of( number( static_cast<i64>( turns ) ) );
            }
            else if( v.kind == Kind::Form && v.f.carried == x )
            {
                Form step    = v.f;
                step.carried = {};
                Form total;
                for( auto& [k, w] : step.terms )
                {
                    Key bound = k;
                    bound.bound |= static_cast<u8>( 1u << s );
                    total.terms.emplace( std::move( bound ), std::move( w ) );
                }
                v = add( before[x.v], of( std::move( total ) ), 1 );
            }
            else if( v.kind == Kind::Form && v.f.carried.is_valid() )
            {
                fail();
            }
            else if( depends( v, s ) )
            {
                v = {};
            }
        }
        for( size_t i = first_store; i < stores_.size(); ++i )
        {
            stores_[i].bound |= static_cast<u8>( 1u << s );
        }
        return test.targets[1];
    }

    void execute( const Statement& s )
    {
        switch( s.kind )
        {
        case Statement_kind::Storage_live:
        case Statement_kind::Storage_dead:
            return;
        case Statement_kind::Drop:
            fail();
        case Statement_kind::Assign:
            write( s.place, evaluate( func_.rvalue( s ) ) );
            return;
        }
    }

    Value evaluate( const Rvalue& r )
    {
        switch( r.kind )
        {
        case Rvalue_kind::Use:
            return operand( r.a );
        case Rvalue_kind::Binary:
            return binary( r, operand( r.a ), operand( r.b ) );
        case Rvalue_kind::Unary:
            return unary( r, operand( r.a ) );
        case Rvalue_kind::Cast:
            return cast( r, operand( r.a ) );
        case Rvalue_kind::Call:
            return call( r );
        default:
            fail();
        }
    }

    std::optional<u64> bits( const Value& v, Type_id type ) const
    {
        if( v.kind == Kind::Real )
        {
            return std::bit_cast<u64>( v.b );
        }
        if( v.kind != Kind::Int || !concrete( v.i ) )
        {
            return std::nullopt;
        }
        const u64 x     = static_cast<u64>( *constant_of( v.i.p ) );
        const u8  width = types_.is_integer( type ) ? types_.get( type ).width : 64;
        return width == 64 ? x : x & ( ( u64 { 1 } << width ) - 1 );
    }

    Value from_bits( u64 x, Type_id type ) const
    {
        if( types_.is_float( type ) )
        {
            if( types_.get( type ).width != 64 )
            {
                fail();
            }
            return real( std::bit_cast<f64>( x ) );
        }
        if( types_.is_integer( type ) )
        {
            const Type& t = types_.get( type );
            if( t.is_signed && t.width < 64 && ( x >> ( t.width - 1 ) & 1 ) != 0 )
            {
                x |= ~u64 { 0 } << t.width;
            }
            if( !t.is_signed && x > static_cast<u64>( std::numeric_limits<i64>::max() ) )
            {
                return {};
            }
        }
        return of( number( static_cast<i64>( x ) ) );
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

    Int within( Int x, Type_id type ) const
    {
        const auto [lo, hi] = limits( type );
        if( x.lo < lo || x.hi > hi )
        {
            fail();
        }
        return x;
    }

    // The exact range of p over the symbols' ranges, which start at 0.
    std::pair<i128, i128> range( const Poly& p ) const
    {
        i128 lo = 0, hi = 0;
        for( const auto& [m, c] : p )
        {
            i128 top = 1;
            for( u32 s = 0; s < k_symbols; ++s )
            {
                for( u8 e = 0; e < m[s]; ++e )
                {
                    top *= static_cast<i128>( turns_[s] ) - 1;
                    if( top > ( i128 { 1 } << 64 ) )
                    {
                        fail();
                    }
                }
            }
            if( m == Monomial {} )
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

    Int made( Poly p, u64 q, i128 lo, i128 hi ) const
    {
        if( q == 0 )
        {
            const auto [l, h] = range( p );
            return { std::move( p ), 0, l, h };
        }
        return { std::move( p ), q, lo, hi };
    }

    Value binary( const Rvalue& r, const Value& x, const Value& y )
    {
        if( x.kind == Kind::Real && y.kind == Kind::Real )
        {
            return reals( r.op, x.b, y.b );
        }
        const std::optional<u64> bx = bits( x, r.a.type );
        const std::optional<u64> by = bits( y, r.b.type );
        if( bx && by )
        {
            const std::optional<u64> folded = keel::evaluate( r, bx, by, types_ );
            return folded ? from_bits( *folded, r.type ) : Value {};
        }
        if( compares( r.op ) )
        {
            return compare( r.op, x, y );
        }
        if( types_.is_float( r.a.type ) )
        {
            return arithmetic( r.op, x, y );
        }
        if( x.kind != Kind::Int || y.kind != Kind::Int )
        {
            fail();
        }
        const Int& a = x.i;
        const Int& b = y.i;
        const u64  q = std::gcd( a.q, b.q );
        switch( r.op )
        {
        case Token_kind::Plus:
            return of( within( made( sum( a.p, b.p ), q, a.lo + b.lo, a.hi + b.hi ), r.type ) );
        case Token_kind::Minus:
            return of( within( made( sum( a.p, b.p, -1 ), q, a.lo - b.hi, a.hi - b.lo ), r.type ) );
        case Token_kind::Star:
        {
            const i128 c[] { a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi };
            return of(
                within( made( product( a.p, b.p ), q, *std::ranges::min_element( c ), *std::ranges::max_element( c ) ), r.type )
            );
        }
        case Token_kind::Percent:
        {
            if( !concrete( b ) || *constant_of( b.p ) <= 0 || a.lo < 0 )
            {
                fail();
            }
            const i64 n = *constant_of( b.p );
            if( a.hi < n )
            {
                return x;
            }
            return of( Int {
                a.p, a.q == 0 ? static_cast<u64>( n ) : std::gcd( a.q, static_cast<u64>( n ) ), 0, std::min<i128>( n - 1, a.hi )
            } );
        }
        default:
            fail();
        }
    }

    static Value reals( Token_kind op, f64 x, f64 y )
    {
        switch( op )
        {
        case Token_kind::Plus:
            return real( x + y );
        case Token_kind::Minus:
            return real( x - y );
        case Token_kind::Star:
            return real( x * y );
        case Token_kind::Slash:
            return real( x / y );
        case Token_kind::Less:
            return truth( x < y );
        case Token_kind::Less_equal:
            return truth( x <= y );
        case Token_kind::Greater:
            return truth( x > y );
        case Token_kind::Greater_equal:
            return truth( x >= y );
        case Token_kind::Equal_equal:
            return truth( x == y );
        case Token_kind::Bang_equal:
            return truth( x != y );
        default:
            fail();
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
    static std::optional<std::pair<f64, f64>> span_of( const Value& v )
    {
        switch( v.kind )
        {
        case Kind::Int:
            return std::pair { static_cast<f64>( v.i.lo ), static_cast<f64>( v.i.hi ) };
        case Kind::Real:
            return std::pair { v.b, v.b };
        case Kind::Angle:
        {
            const f64 l = v.a * static_cast<f64>( v.i.lo ) + v.b;
            const f64 h = v.a * static_cast<f64>( v.i.hi ) + v.b;
            return std::pair { std::min( l, h ), std::max( l, h ) };
        }
        default:
            return std::nullopt;
        }
    }

    static Value truth( bool t )
    {
        return of( number( t ? 1 : 0 ) );
    }

    // Decided from the ranges, or by being the same value.
    static Value compare( Token_kind op, const Value& x, const Value& y )
    {
        const bool equal = x.kind == y.kind && ( ( x.kind == Kind::Int && same( x.i, y.i ) ) ||
                                                 ( x.kind == Kind::Angle && same( x.i, y.i ) && x.a == y.a && x.b == y.b ) );
        const auto sx    = span_of( x );
        const auto sy    = span_of( y );
        if( !sx || !sy )
        {
            return {};
        }
        const auto [xl, xh] = *sx;
        const auto [yl, yh] = *sy;
        switch( op )
        {
        case Token_kind::Less:
            return xh < yl ? truth( true ) : xl >= yh ? truth( false ) : Value {};
        case Token_kind::Less_equal:
            return xh <= yl ? truth( true ) : xl > yh ? truth( false ) : Value {};
        case Token_kind::Greater:
            return compare( Token_kind::Less, y, x );
        case Token_kind::Greater_equal:
            return compare( Token_kind::Less_equal, y, x );
        case Token_kind::Equal_equal:
            return equal ? truth( true ) : xh < yl || yh < xl ? truth( false ) : Value {};
        case Token_kind::Bang_equal:
            return equal ? truth( false ) : xh < yl || yh < xl ? truth( true ) : Value {};
        default:
            return {};
        }
    }

    static Wave as_wave( const Value& v )
    {
        if( v.kind == Kind::Real )
        {
            return wave( v.b );
        }
        if( v.kind != Kind::Wave )
        {
            fail();
        }
        return v.w;
    }

    static Form as_form( const Value& v )
    {
        if( v.kind == Kind::Form )
        {
            return v.f;
        }
        if( v.kind == Kind::Real && v.b == 0 )
        {
            return {};
        }
        fail();
    }

    Value add( const Value& x, const Value& y, f64 sign )
    {
        if( x.kind == Kind::Form || y.kind == Kind::Form )
        {
            return of( sum( as_form( x ), as_form( y ), sign ) );
        }
        if( x.kind == Kind::Angle || y.kind == Kind::Angle )
        {
            if( x.kind == Kind::Angle && y.kind == Kind::Real )
            {
                return { .kind = Kind::Angle, .i = x.i, .a = x.a, .b = x.b + sign * y.b };
            }
            if( x.kind == Kind::Real && y.kind == Kind::Angle )
            {
                return { .kind = Kind::Angle, .i = y.i, .a = sign * y.a, .b = x.b + sign * y.b };
            }
            if( x.kind == Kind::Angle && y.kind == Kind::Angle && same( x.i, y.i ) )
            {
                return { .kind = Kind::Angle, .i = x.i, .a = x.a + sign * y.a, .b = x.b + sign * y.b };
            }
            fail();
        }
        return of( sum( as_wave( x ), as_wave( y ), sign ) );
    }

    Value arithmetic( Token_kind op, const Value& x, const Value& y )
    {
        Value v;
        switch( op )
        {
        case Token_kind::Plus:
        case Token_kind::Minus:
            v = add( x, y, op == Token_kind::Plus ? 1 : -1 );
            break;
        case Token_kind::Star:
            if( x.kind == Kind::Form && y.kind == Kind::Form )
            {
                fail();
            }
            if( x.kind == Kind::Form || y.kind == Kind::Form )
            {
                v = of( x.kind == Kind::Form ? scaled( x.f, as_wave( y ) ) : scaled( y.f, as_wave( x ) ) );
            }
            else if( x.kind == Kind::Angle && y.kind == Kind::Real )
            {
                v = { .kind = Kind::Angle, .i = x.i, .a = x.a * y.b, .b = x.b * y.b };
            }
            else if( x.kind == Kind::Real && y.kind == Kind::Angle )
            {
                v = { .kind = Kind::Angle, .i = y.i, .a = x.b * y.a, .b = x.b * y.b };
            }
            else
            {
                v = of( product( as_wave( x ), as_wave( y ) ) );
            }
            break;
        case Token_kind::Slash:
            if( y.kind != Kind::Real || y.b == 0 )
            {
                fail();
            }
            if( x.kind == Kind::Angle )
            {
                v = { .kind = Kind::Angle, .i = x.i, .a = x.a / y.b, .b = x.b / y.b };
            }
            else if( x.kind == Kind::Form )
            {
                v = of( scaled( x.f, wave( 1 / y.b ) ) );
            }
            else
            {
                v = of( scaled( as_wave( x ), 1 / y.b ) );
            }
            break;
        default:
            fail();
        }
        if( v.kind == Kind::Form )
        {
            ( op == Token_kind::Star || op == Token_kind::Slash ? muls_ : adds_ ) += multiplicity_;
        }
        return v;
    }

    Value unary( const Rvalue& r, const Value& x )
    {
        if( const std::optional<u64> b = bits( x, r.a.type ) )
        {
            const std::optional<u64> folded = keel::evaluate( r, b, std::nullopt, types_ );
            return folded ? from_bits( *folded, r.type ) : Value {};
        }
        if( r.op != Token_kind::Minus )
        {
            fail();
        }
        switch( x.kind )
        {
        case Kind::Int:
            return of( within( made( sum( {}, x.i.p, -1 ), x.i.q, -x.i.hi, -x.i.lo ), r.type ) );
        case Kind::Angle:
            return { .kind = Kind::Angle, .i = x.i, .a = -x.a, .b = -x.b };
        case Kind::Wave:
            return of( scaled( x.w, -1 ) );
        case Kind::Form:
            return of( scaled( x.f, wave( -1 ) ) );
        default:
            fail();
        }
    }

    Value cast( const Rvalue& r, const Value& x )
    {
        if( const std::optional<u64> b = bits( x, r.a.type ) )
        {
            if( const std::optional<u64> folded = keel::evaluate( r, b, std::nullopt, types_ ) )
            {
                return from_bits( *folded, r.type );
            }
        }
        const bool from_int = types_.is_integer( r.a.type );
        const bool to_int   = types_.is_integer( r.type );
        if( from_int && types_.is_float( r.type ) && x.kind == Kind::Int && concrete( x.i ) )
        {
            const i64 v = *constant_of( x.i.p );
            if( types_.get( r.type ).width != 64 || v < -( i64 { 1 } << 53 ) || v > ( i64 { 1 } << 53 ) )
            {
                fail();
            }
            return real( static_cast<f64>( v ) );
        }
        if( from_int && to_int && x.kind == Kind::Int )
        {
            return of( within( x.i, r.type ) );
        }
        if( from_int && types_.is_float( r.type ) && types_.get( r.type ).width == 64 && x.kind == Kind::Int )
        {
            const i128 exact = i128 { 1 } << 53;
            if( x.i.lo < -exact || x.i.hi > exact )
            {
                fail();
            }
            return { .kind = Kind::Angle, .i = x.i, .a = 1, .b = 0 };
        }
        if( to_int && x.kind == Kind::Real && x.b == std::trunc( x.b ) && std::abs( x.b ) < 0x1p53 )
        {
            return of( within( number( static_cast<i64>( x.b ) ), r.type ) );
        }
        if( to_int && x.kind == Kind::Angle && x.a == 1 && x.b == 0 )
        {
            return of( within( x.i, r.type ) );
        }
        if( r.a.type == r.type )
        {
            return x;
        }
        fail();
    }

    // A `@trig_*` call at an angle 2 pi (p/q i + p'/q'): powers of W.
    Value call( const Rvalue& r )
    {
        const std::optional<u32> c = purity_.callee( r );
        if( !c || r.arguments.size() != 1 )
        {
            fail();
        }
        const Trig kind = functions_[*c].trig;
        if( kind != Trig::Sin && kind != Trig::Cos )
        {
            fail();
        }
        Value x = operand( r.arguments[0] );
        if( x.kind == Kind::Real )
        {
            x = { .kind = Kind::Angle, .i = number( 0 ), .a = 0, .b = x.b };
        }
        if( x.kind != Kind::Angle )
        {
            fail();
        }
        const auto r1 = rational( x.a / ( 2 * std::numbers::pi ) );
        const auto r0 = rational( x.b / ( 2 * std::numbers::pi ) );
        if( !r1 || !r0 || ( x.i.q != 0 && i128 { r1->first } * x.i.q % static_cast<i128>( r1->second ) != 0 ) )
        {
            fail();
        }
        const u64 M = common( r1->second, r0->second );
        // e^(i angle) = W_M^e, W_M = e^(-2 pi i / M).
        const Poly e = reduced(
            sum( product( x.i.p, constant( narrowed( -i128 { r1->first } * static_cast<i128>( M / r1->second ) ) ) ),
                 constant( narrowed( -i128 { r0->first } * static_cast<i128>( M / r0->second ) ) ) ),
            M
        );
        const Poly minus = reduced( sum( {}, e, -1 ), M );
        Wave       w { M, {} };
        if( kind == Trig::Cos )
        {
            add_term( w, e, 0.5 );
            add_term( w, minus, 0.5 );
        }
        else
        {
            add_term( w, e, std::complex<f64> { 0, -0.5 } );
            add_term( w, minus, std::complex<f64> { 0, 0.5 } );
        }
        return of( std::move( w ) );
    }

    Value operand( const Operand& o )
    {
        switch( o.kind )
        {
        case Operand_kind::Constant:
            if( types_.is_float( o.type ) )
            {
                if( types_.get( o.type ).width != 64 )
                {
                    fail();
                }
                return real( literals_.floating( o.constant ) );
            }
            return from_bits( literals_.integer( o.constant ), o.type );
        case Operand_kind::Copy:
        case Operand_kind::Move:
            return read( o.place, o.type );
        case Operand_kind::Debug:
            fail();
        }
        fail();
    }

    // The stream `at` names and the element's index, for a parameter's element.
    std::pair<u32, Poly> element( const Place& at )
    {
        if( at.is_global() || !at.local.is_valid() || at.local.v == 0 || at.local.v > func_.parameter_count )
        {
            fail();
        }
        const Run<const Projection> steps = func_.projections_of( at );
        u32                         i     = 0;
        Stream                      s { .local = at.local };
        if( i < steps.size() && steps[i].kind == Projection_kind::Deref )
        {
            s.through = true;
            ++i;
        }
        if( i >= steps.size() || steps[i].kind != Projection_kind::Index )
        {
            fail();
        }
        const Value& index = locals_[steps[i].index.v];
        if( index.kind != Kind::Int || index.i.q != 0 || index.i.lo < 0 )
        {
            fail();
        }
        for( ++i; i < steps.size(); ++i )
        {
            if( steps[i].kind != Projection_kind::Field )
            {
                fail();
            }
            s.fields.push_back( steps[i].field );
        }
        for( u32 k = 0; k < streams_.size(); ++k )
        {
            if( streams_[k].local == s.local && streams_[k].through == s.through && streams_[k].fields == s.fields )
            {
                return { k, index.i.p };
            }
        }
        streams_.push_back( std::move( s ) );
        return { static_cast<u32>( streams_.size() - 1 ), index.i.p };
    }

    Value read( const Place& at, Type_id type )
    {
        if( at.is_whole_local() )
        {
            return locals_[at.local.v];
        }
        if( !types_.is_float( type ) || types_.get( type ).width != 64 )
        {
            fail();
        }
        auto [stream, index] = element( at );
        if( streams_[stream].written )
        {
            fail();
        }
        streams_[stream].read = true;
        Form f;
        f.terms.emplace( Key { stream, std::move( index ), 0 }, wave( 1 ) );
        return of( std::move( f ) );
    }

    void write( const Place& at, Value v )
    {
        if( at.is_whole_local() )
        {
            locals_[at.local.v] = std::move( v );
            return;
        }
        auto [stream, index] = element( at );
        if( streams_[stream].read )
        {
            fail();
        }
        streams_[stream].written = true;
        Form f                   = as_form( v );
        if( f.carried.is_valid() )
        {
            fail();
        }
        stores_.push_back( { .stream = stream, .index = std::move( index ), .value = std::move( f ), .bound = 0 } );
    }

    static std::optional<u32> one_bit( u8 mask )
    {
        return std::popcount( mask ) == 1 ? std::optional<u32>( static_cast<u32>( std::countr_zero( mask ) ) ) : std::nullopt;
    }

    // Each input or output stream's place for element e.
    Value_path path( u32 a, u32 b, u64 e ) const
    {
        const auto one = [&]( const Stream& s )
        {
            Param_path p { .local = s.local, .through = s.through, .steps = {} };
            p.steps.push_back( { .index = true, .element = e } );
            for( const Node_id f : s.fields )
            {
                p.steps.push_back( { .field = f } );
            }
            return p;
        };
        return { one( streams_[a] ), one( streams_[b] ) };
    }

    // y = x * K, complex, from the four real forms.
    std::optional<Read_kernel> collect() const
    {
        std::vector<u32> in, out;
        for( u32 k = 0; k < streams_.size(); ++k )
        {
            ( streams_[k].written ? out : in ).push_back( k );
        }
        const auto order = [&]( u32 a, u32 b )
        { return std::pair { streams_[a].local, streams_[a].fields } < std::pair { streams_[b].local, streams_[b].fields }; };
        std::ranges::sort( in, order );
        std::ranges::sort( out, order );
        if( in.size() != 2 || out.size() != 2 || stores_.size() != 2 )
        {
            return std::nullopt;
        }

        const Store* stored[2] {};
        for( const Store& s : stores_ )
        {
            stored[s.stream == out[0] ? 0 : 1] = &s;
        }
        if( stored[0] == nullptr || stored[1] == nullptr || stored[0]->bound != stored[1]->bound )
        {
            return std::nullopt;
        }
        const std::optional<u32> k = one_bit( stored[0]->bound );
        if( !k || stored[0]->index != symbol( *k ) || stored[1]->index != symbol( *k ) )
        {
            return std::nullopt;
        }

        std::optional<u32> j;
        Wave               c[2][2];
        for( u32 o = 0; o < 2; ++o )
        {
            for( const auto& [key, w] : stored[o]->value.terms )
            {
                const std::optional<u32> b = one_bit( key.bound );
                if( !b || *b == *k || ( j && *b != *j ) || key.index != symbol( *b ) )
                {
                    return std::nullopt;
                }
                j = b;
                for( const auto& [e, coefficient] : w.terms )
                {
                    for( u32 s = 0; s < k_symbols; ++s )
                    {
                        if( s != *k && s != *j && depends( e, s ) )
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
        if( !sum( c[0][0], c[1][1], -1 ).terms.empty() || !sum( c[0][1], c[1][0] ).terms.empty() )
        {
            return std::nullopt;
        }
        const Wave K = sum( c[0][0], c[1][0], std::complex<f64> { 0, 1 } );
        if( K.terms.size() != 1 || std::abs( K.terms.begin()->second - 1.0 ) > 1e-9 )
        {
            return std::nullopt;
        }
        const Poly& e = K.terms.begin()->first;
        Monomial    jk {};
        jk[*j] = 1;
        jk[*k] = 1;
        if( e.size() > 1 || ( e.size() == 1 && e.begin()->first != jk ) )
        {
            return std::nullopt;
        }

        Read_kernel r {
            .kernel =
                { .n = static_cast<u32>( turns_[*j] ),
                  .m = static_cast<u32>( turns_[*k] ),
                  .M = K.M,
                  .c = e.empty() ? 0 : static_cast<u64>( e.begin()->second ) },
            .input =
                { static_cast<u32>( std::min<u64>( adds_, ~u32 { 0 } ) ), static_cast<u32>( std::min<u64>( muls_, ~u32 { 0 } ) )
                },
        };
        for( u64 x = 0; x < r.kernel.n; ++x )
        {
            r.inputs.push_back( path( in[0], in[1], x ) );
        }
        for( u64 y = 0; y < r.kernel.m; ++y )
        {
            r.outputs.push_back( path( out[0], out[1], y ) );
        }
        return r;
    }

    const Function&              func_;
    const std::vector<Function>& functions_;
    const Purity&                purity_;
    const Type_table&            types_;
    const Literal_pool&          literals_;
    const Loops                  loops_;
    const Local_set              addressed_;

    std::vector<Value>         locals_;
    std::vector<Stream>        streams_;
    std::vector<Store>         stores_;
    std::array<u64, k_symbols> turns_ {};
    u32                        symbols_      = 0;
    u64                        steps_        = 0;
    u64                        multiplicity_ = 1;
    u64                        adds_         = 0;
    u64                        muls_         = 0;
};
} // namespace

std::optional<Read_kernel> read_kernel(
    const Function&              func,
    const std::vector<Function>& functions,
    const Purity&                purity,
    const Type_table&            types,
    const Literal_pool&          literals
)
{
    return Reader( func, functions, purity, types, literals ).run();
}
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include "check/pipeline_test_support.h"
#include "ir/call_graph.h"

namespace keel
{
namespace
{
// At -O3 without the loop phase, where the pass runs; the function under test is the first.
std::optional<Read_kernel> read_first( std::string_view source )
{
    const Compiled c(
        std::string( source ) + "@trig_cos f64 cosine( f64 x ) { return x; }\n@trig_sin f64 sine( f64 x ) { return x; }\ni32 "
                                "main() { return 0; }\n",
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

TEST_CASE( "a DFT loop nest reads as its kernel", "[egraph][read_kernel]" )
{
    const std::optional<Read_kernel> r = read_first( dft( "-2.0 * 3.141592653589793 * cast<f64>( j * k % 96 ) / 96.0" ) );
    REQUIRE( r );
    CHECK( r->kernel == Kernel { 96, 96, 96, 1 } );
    CHECK( r->inputs.size() == 96 );
    CHECK( r->outputs.size() == 96 );
    CHECK( r->input.muls == 4 * 96 * 96 );
}

TEST_CASE( "an inverse DFT reads as c = M - 1, without the remainder too", "[egraph][read_kernel]" )
{
    const std::optional<Read_kernel> r = read_first( dft( "2.0 * 3.141592653589793 * cast<f64>( j * k ) / 96.0" ) );
    REQUIRE( r );
    CHECK( r->kernel == Kernel { 96, 96, 96, 95 } );
}

TEST_CASE( "a loop that is not one complex kernel reads as none", "[egraph][read_kernel]" )
{
    // Two inputs multiplied.
    CHECK( !read_first( dft( "-2.0 * 3.141592653589793 * cast<f64>( j * k ) / 96.0", "sr += re[j] * im[j]; si += im[j] * c;" ) )
    );
    // Not a rotation: cos on both parts.
    CHECK( !read_first( dft( "-2.0 * 3.141592653589793 * cast<f64>( j * k ) / 96.0", "sr += re[j] * c; si += im[j] * s;" ) ) );
    // An angle quadratic in j is no c j k kernel.
    CHECK( !read_first( dft( "-2.0 * 3.141592653589793 * cast<f64>( j * j * k ) / 96.0" ) ) );
}
} // namespace keel
#endif
