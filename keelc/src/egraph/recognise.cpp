// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "egraph/recognise.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>
#include <unordered_map>
#include "opt/fold_constants.h"

namespace keel
{
namespace
{
constexpr u32 k_largest_root  = 256;  // the largest M a constant or an angle is matched at
constexpr u32 k_largest_field = 1024; // the largest M a coefficient may need
constexpr u32 k_deepest_call  = 64;

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
};

struct Value
{
    Kind  kind  = Kind::Unknown;
    u64   bits  = 0;     // Number: an integer's bits, or an f64's
    bool  exact = false; // Number: an f64 whose exact value is `value`
    Exact value {};
    u32   index = 0; // Form: into forms_; Address: into locations_
};

// A root, then fields (odd) and elements (even).
struct Location
{
    u32              root = 0;
    std::vector<u64> steps;

    auto operator<=>( const Location& ) const = default;
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
        u64                          budget
    )
        : functions_( functions ),
          purity_( purity ),
          types_( types ),
          literals_( literals ),
          budget_( budget )
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

    // A short dyadic as it stands, else within rounding of 2^s cos(2 pi k / M).
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
        if( roots_[from.root].input )
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

    Location locate( Frame& fr, const Place& place )
    {
        if( place.is_global() )
        {
            end( Ending::Global );
        }
        Location at { root( fr, place.local ), {} };
        Type_id  type = fr.func->locals[place.local].type;
        for( const Projection& p : fr.func->projections_of( place ) )
        {
            switch( p.kind )
            {
            case Projection_kind::Field:
                at.steps.push_back( u64 { p.field.v } << 1 | 1 );
                break;
            case Projection_kind::Index:
            {
                const Value i = read( Location { root( fr, p.index ), {} }, fr.func->locals[p.index].type );
                if( i.kind != Kind::Number )
                {
                    end( Ending::Branch );
                }
                at.steps.push_back( i.bits << 1 );
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
            if( !is_zero( n ) )
            {
                end( Ending::Constant );
            }
            count( false );
            if( left || op == Token_kind::Plus )
            {
                return left ? a : b;
            }
            return form( scaled( forms_[b.index], negative_one() ) );
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
            write( to, trig( fr, r, g.trig ) );
            return;
        }
        if( !purity_.pure( r ) )
        {
            bool symbolic = false;
            for( const Operand& argument : r.arguments )
            {
                symbolic = symbolic || operand( fr, argument ).kind == Kind::Form;
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
            if( a.kind == Kind::Form || b.kind == Kind::Form )
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
            if( a.kind == Kind::Form )
            {
                end( Ending::Bits );
            }
            return a.kind == Kind::Number ? number( a.bits ) : Value {};
        }
        case Rvalue_kind::Sqrt:
        {
            const Value a = operand( fr, r.a );
            if( a.kind == Kind::Form )
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
            end( v.kind == Kind::Form ? Ending::Escape : Ending::Effect );
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

    void execute( Frame& fr )
    {
        ++depth_;
        Block_id b = k_entry_block;
        for( ;; )
        {
            for( const Statement& s : fr.func->statements_of( b ) )
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
                step & 1 ? Step { .field = Node_id { static_cast<u32>( step >> 1 ) } }
                         : Step { .index = true, .element = step >> 1 }
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
            else
            {
                end( cell.value.kind == Kind::Number ? Ending::Constant : Ending::Unknown, cell.span );
            }
            outs.push_back( at );
        }
        if( outs.empty() || inputs_.empty() )
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
        for( const Pairs& rp : pairings( static_cast<u32>( outs.size() ) ) )
        {
            for( const Pairs& cp : pairings( cols ) )
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
                out.inputs.push_back( { path( at ), {} } );
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
            out.rows.push_back( std::move( row ) );
            out.outputs.push_back( { path( outs[p] ), path( outs[q] ) } );
        }
        for( const auto& [u, v] : paired->second )
        {
            out.inputs.push_back( { path( ins[u] ), path( ins[v] ) } );
        }
        return out;
    }

    const std::vector<Function>& functions_;
    const Purity&                purity_;
    const Type_table&            types_;
    const Literal_pool&          literals_;
    u64                          budget_;
    u64                          steps_ = 0;
    u32                          depth_ = 0;
    Span                         span_ {};

    std::map<u32, Field>                          fields_;
    std::unordered_map<u64, Element>              lifts_;
    std::unordered_map<u64, std::optional<Exact>> literal_exacts_;
    std::vector<Root>                             roots_;
    std::vector<Location>                         locations_;
    std::map<Location, Cell>                      memory_; // what the run wrote
    std::map<Location, Value>                     read_;   // inputs not yet overwritten
    std::vector<Form>                             forms_;
    std::vector<Location>                         inputs_; // in the order first read
    bool                                          ring_    = false;
    bool                                          integer_ = false;
    u8                                            width_   = 64;
    Type_id                                       type_ {};
    Cost                                          cost_ {};
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
    case Ending::Constant:
        return "adds a constant to an input, or writes one to an output";
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

std::variant<Recognised, Unrecognised> recognise(
    const Function&              func,
    const std::vector<Function>& functions,
    const Purity&                purity,
    const Type_table&            types,
    const Literal_pool&          literals,
    u64                          budget
)
{
    return Recogniser( functions, purity, types, literals, budget ).run( func );
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
        result = recognise( functions.front(), all, purity, types.table(), literals, budget );
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
} // namespace

TEST_CASE( "recognise_pairs_a_dft4_loop", "[egraph][recognise]" )
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

TEST_CASE( "recognise_pairs_a_dft4_by_hand", "[egraph][recognise]" )
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

TEST_CASE( "recognise_reads_an_integer_wht4_in_place", "[egraph][recognise]" )
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

TEST_CASE( "recognise_reads_cos_constants_and_roots_exactly", "[egraph][recognise]" )
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

TEST_CASE( "recognise_ends_with_the_operation_that_stops_it", "[egraph][recognise]" )
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
    CHECK( ending( "f64 f( f64 x ) { return x * 0.1; }\n" ) == Ending::Coefficient );
    CHECK( ending( "f64 f( f64 x ) { return x + 1.0; }\n" ) == Ending::Constant );
    CHECK(
        ending( "f64 f( f64 x ) { f64 s = 0.0; for( u64 i = 0; i < 1000; i += 1 ) { s += x; } return s; }\n", 100 ) ==
        Ending::Budget
    );
}
} // namespace keel
#endif // ENABLE_UNIT_TESTS
