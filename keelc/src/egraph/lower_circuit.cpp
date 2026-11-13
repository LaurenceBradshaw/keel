// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "egraph/lower_circuit.h"
#include <cassert>
#include <cmath>
#include <random>
#include <unordered_map>
#include <utility>

namespace keel
{
namespace
{
// A float, or an integer's bits.
struct Number
{
    f64 f    = 0;
    u64 bits = 0;
};

// Float or wrapping integer arithmetic on Numbers.
struct Arithmetic
{
    bool integer;
    u32  width;

    Number wrap( u64 bits ) const
    {
        return { 0, truncate( bits, static_cast<u8>( width ) ) };
    }

    Number plus( Number a, Number b ) const
    {
        return integer ? wrap( a.bits + b.bits ) : Number { a.f + b.f };
    }

    Number minus( Number a, Number b ) const
    {
        return integer ? wrap( a.bits - b.bits ) : Number { a.f - b.f };
    }

    Number times( Number a, Number b ) const
    {
        return integer ? wrap( a.bits * b.bits ) : Number { a.f * b.f };
    }

    Number negated( Number a ) const
    {
        return integer ? wrap( 0 - a.bits ) : Number { -a.f };
    }

    bool zero( Number a ) const
    {
        return integer ? a.bits == 0 : a.f == 0;
    }

    bool negative( Number a ) const
    {
        return integer ? as_signed( a.bits, static_cast<u8>( width ) ) < 0 : a.f < 0;
    }

    Number magnitude( Number a ) const
    {
        return negative( a ) ? negated( a ) : a;
    }

    bool one( Number a ) const
    {
        return integer ? magnitude( a ).bits == 1 : std::abs( a.f ) == 1;
    }

    bool same_magnitude( Number a, Number b ) const
    {
        return integer ? magnitude( a ).bits == magnitude( b ).bits : std::abs( std::abs( a.f ) - std::abs( b.f ) ) < 1e-12;
    }
};

Arithmetic arithmetic_of( const Field& field )
{
    return { field.kind() == Ring_kind::Integer, field.width() };
}

bool paired( const Field& field )
{
    return field.kind() == Ring_kind::Complex || ( field.kind() == Ring_kind::Integer && field.n() == 4 );
}

// Real and imaginary parts of an element; an integer ring's W_4 is -i.
std::pair<Number, Number> parts( const Field& field, Element e )
{
    if( field.kind() != Ring_kind::Integer )
    {
        const std::complex<f64> v = field.value( e );
        return { { v.real() }, { v.imag() } };
    }
    const Arithmetic z = arithmetic_of( field );
    const Number     a = z.wrap( static_cast<u64>( field.coefficient( e, 0 ).first ) );
    const Number     b = field.degree() > 1 ? z.wrap( static_cast<u64>( field.coefficient( e, 1 ).first ) ) : Number {};
    return { a, z.negated( b ) };
}

class Lowerer
{
public:
    Lowerer( Function& func, const Field& field, Type_id type, Literal_pool& literals, Span span )
        : func_( func ),
          field_( field ),
          z_( arithmetic_of( field ) ),
          paired_( paired( field ) ),
          type_( type ),
          literals_( literals ),
          span_( span )
    {
    }

    std::vector<Statement> lower(
        const Circuit&                  circuit,
        const std::vector<Row>&         rows,
        const std::vector<Value_place>& inputs,
        const std::vector<Value_place>& outputs
    )
    {
        assert( rows.size() == outputs.size() );
        const std::vector<Op>&  ops  = circuit.ops();
        const std::vector<bool> live = circuit.live();
        std::vector<Value>      v( ops.size() );
        for( u32 o = 0; o < ops.size(); ++o )
        {
            if( live[o] && ops[o].kind == Op_kind::Input )
            {
                v[o] = { load( inputs[ops[o].a].real ), paired_ ? load( inputs[ops[o].a].imaginary ) : Part {} };
            }
        }
        for( u32 o = 0; o < ops.size(); ++o )
        {
            if( !live[o] || ops[o].kind == Op_kind::Input )
            {
                continue;
            }
            const Value& a = v[ops[o].a];
            if( ops[o].kind == Op_kind::Add )
            {
                const Value& b = v[ops[o].b];
                v[o]           = { plus( a.re, b.re ), plus( a.im, b.im ) };
                continue;
            }

            const auto [cr, ci] = z_.integer
                                      ? parts( field_, circuit.exact( ops[o].b ) )
                                      : std::pair<Number, Number> {
                                            { circuit.constant( ops[o].b ).real() }, { circuit.constant( ops[o].b ).imag() }
                                        };
            v[o]                = scale( a, cr, ci );
        }
        for( size_t i = 0; i < rows.size(); ++i )
        {
            const auto  found = circuit.outputs.find( rows[i] );
            const Value y     = found == circuit.outputs.end() ? Value {} : v[found->second];
            store( outputs[i].real, y.re );
            if( paired_ )
            {
                store( outputs[i].imaginary, y.im );
            }
        }
        return std::move( out_ );
    }

private:
    // A local's value, negated or not; no local is zero.
    struct Part
    {
        Local_id local {};
        bool     negated = false;
    };

    struct Value
    {
        Part re;
        Part im;
    };

    Operand read( Part p ) const
    {
        return copy( Place { .local = p.local }, type_ );
    }

    Operand literal( Number k ) const
    {
        return constant(
            z_.integer ? literals_.add_integer( k.bits ) : literals_.add_float( k.f, static_cast<f32>( k.f ) ), type_
        );
    }

    void assign( Place place, Rvalue value )
    {
        out_.push_back( Statement { .span = span_, .place = place, .value = func_.add_rvalue( std::move( value ) ) } );
    }

    Part emit( Rvalue value )
    {
        const Local_id t = func_.locals.push_back( Local { .type = type_, .span = span_ } );
        assign( Place { .local = t }, std::move( value ) );
        return { t };
    }

    Part load( const Place& place )
    {
        return emit( use( copy( place, type_ ) ) );
    }

    void store( const Place& place, Part p )
    {
        if( !p.local.is_valid() )
        {
            assign( place, use( literal( {} ) ) );
        }
        else
        {
            assign( place, p.negated ? unary( Token_kind::Minus, read( p ), type_ ) : use( read( p ) ) );
        }
    }

    Part times( Number k, Part p )
    {
        if( !p.local.is_valid() || z_.zero( k ) )
        {
            return {};
        }
        const bool negated = p.negated != z_.negative( k );
        if( !z_.one( k ) )
        {
            p = emit( binary( Token_kind::Star, read( p ), literal( z_.magnitude( k ) ), type_ ) );
        }
        return { p.local, negated };
    }

    Part plus( Part a, Part b )
    {
        if( !a.local.is_valid() || !b.local.is_valid() )
        {
            return a.local.is_valid() ? a : b;
        }
        if( a.negated == b.negated )
        {
            return { emit( binary( Token_kind::Plus, read( a ), read( b ), type_ ) ).local, a.negated };
        }
        if( a.negated )
        {
            std::swap( a, b );
        }
        return emit( binary( Token_kind::Minus, read( a ), read( b ), type_ ) );
    }

    // (cr + ci i)(re + im i); k(+-1 +-i) takes its sums first, so 2 muls and not 4.
    Value scale( Value x, Number cr, Number ci )
    {
        if( !paired_ )
        {
            return { times( cr, x.re ), {} };
        }
        if( !z_.zero( cr ) && !z_.zero( ci ) && !z_.one( cr ) && z_.same_magnitude( cr, ci ) )
        {
            const Number k  = z_.magnitude( cr );
            const Number sr = z_.negative( cr ) ? z_.negated( one() ) : one();
            const Number si = z_.negative( ci ) ? z_.negated( one() ) : one();
            return {
                times( k, plus( times( sr, x.re ), times( z_.negated( si ), x.im ) ) ),
                times( k, plus( times( sr, x.im ), times( si, x.re ) ) ),
            };
        }
        return {
            plus( times( cr, x.re ), times( z_.negated( ci ), x.im ) ),
            plus( times( cr, x.im ), times( ci, x.re ) ),
        };
    }

    Number one() const
    {
        return z_.integer ? Number { 0, 1 } : Number { 1 };
    }

    Function&              func_;
    const Field&           field_;
    Arithmetic             z_;
    bool                   paired_;
    Type_id                type_;
    Literal_pool&          literals_;
    Span                   span_;
    std::vector<Statement> out_;
};

// Runs straight-line statements of uses, negations, sums, differences and products.
class Machine
{
public:
    Machine( const Function& func, Arithmetic z, const Literal_pool& literals )
        : func_( func ),
          z_( z ),
          literals_( literals )
    {
    }

    void set( const Place& place, Number n )
    {
        if( place.is_whole_local() )
        {
            locals_[place.local.v] = n;
            return;
        }
        for( auto& [p, value] : places_ )
        {
            if( same_place( func_, p, place ) )
            {
                value = n;
                return;
            }
        }
        places_.emplace_back( place, n );
    }

    Number get( const Place& place ) const
    {
        if( place.is_whole_local() )
        {
            return locals_.at( place.local.v );
        }
        for( const auto& [p, value] : places_ )
        {
            if( same_place( func_, p, place ) )
            {
                return value;
            }
        }
        assert( false && "a place read before it was written" );
        return {};
    }

    void run( const std::vector<Statement>& body )
    {
        for( const Statement& s : body )
        {
            const Rvalue& r = func_.rvalue( s );
            const Number  a = operand( r.a );
            switch( r.kind )
            {
            case Rvalue_kind::Use:
                set( s.place, a );
                break;
            case Rvalue_kind::Unary:
                set( s.place, z_.negated( a ) );
                break;
            default:
            {
                const Number b = operand( r.b );
                set( s.place,
                     r.op == Token_kind::Plus    ? z_.plus( a, b )
                     : r.op == Token_kind::Minus ? z_.minus( a, b )
                                                 : z_.times( a, b ) );
            }
            }
        }
    }

private:
    Number operand( const Operand& o ) const
    {
        if( o.has_place() )
        {
            return get( o.place );
        }
        return z_.integer ? Number { 0, literals_.integer( o.constant ) } : Number { literals_.floating( o.constant ) };
    }

    const Function&                       func_;
    Arithmetic                            z_;
    const Literal_pool&                   literals_;
    std::unordered_map<u32, Number>       locals_;
    std::vector<std::pair<Place, Number>> places_;
};

// The body computes every row on random input: exactly in an integer ring, else to rounding.
[[maybe_unused]] bool computes(
    const Function&                 func,
    const std::vector<Statement>&   body,
    const Field&                    field,
    const std::vector<Row>&         rows,
    const std::vector<Value_place>& inputs,
    const std::vector<Value_place>& outputs,
    const Literal_pool&             literals
)
{
    const Arithmetic z   = arithmetic_of( field );
    const bool       two = paired( field );
    Machine          machine( func, z, literals );
    std::mt19937     rng( 7 );
    const auto       draw = [&]
    {
        const u64 high = rng();
        const u64 low  = rng();
        return z.integer ? z.wrap( high << 32 | low ) : Number { static_cast<f64>( low ) / 2147483648.0 - 1 };
    };

    std::vector<std::pair<Number, Number>> x;
    for( const Value_place& in : inputs )
    {
        x.emplace_back( draw(), two ? draw() : Number {} );
        machine.set( in.real, x.back().first );
        if( two )
        {
            machine.set( in.imaginary, x.back().second );
        }
    }
    machine.run( body );

    for( size_t i = 0; i < rows.size(); ++i )
    {
        Number want_re, want_im;
        f64    size = 1;
        for( size_t j = 0; j < x.size(); ++j )
        {
            const auto [cr, ci] = parts( field, rows[i][j] );
            const auto [xr, xi] = x[j];
            want_re             = z.plus( want_re, z.minus( z.times( cr, xr ), z.times( ci, xi ) ) );
            want_im             = z.plus( want_im, z.plus( z.times( cr, xi ), z.times( ci, xr ) ) );
            size += std::abs( field.value( rows[i][j] ) ) * 2;
        }
        const auto close = [&]( Number got, Number want )
        { return z.integer ? got.bits == want.bits : std::abs( got.f - want.f ) <= 1e-9 * size; };
        if( !close( machine.get( outputs[i].real ), want_re ) ||
            ( two && !close( machine.get( outputs[i].imaginary ), want_im ) ) )
        {
            return false;
        }
    }
    return true;
}
} // namespace

std::vector<Statement> lower_circuit(
    Function&                       func,
    const Circuit&                  circuit,
    const Field&                    field,
    const std::vector<Row>&         rows,
    const std::vector<Value_place>& inputs,
    const std::vector<Value_place>& outputs,
    Type_id                         type,
    Literal_pool&                   literals,
    Span                            span
)
{
    std::vector<Statement> body = Lowerer( func, field, type, literals, span ).lower( circuit, rows, inputs, outputs );
    assert( computes( func, body, field, rows, inputs, outputs, literals ) && "a lowered circuit disagrees with its map" );
    return body;
}
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include "egraph/solve.h"

namespace keel
{
namespace
{
struct Lowered
{
    Function                 func;
    std::vector<Value_place> in;
    std::vector<Value_place> out;
    std::vector<Statement>   body;
};

// Each value in whole locals of its own, or `out` the same places as `in`.
Lowered lowered( Field& field, const std::vector<Row>& rows, Type_id type, Literal_pool& literals, bool in_place = false )
{
    Lowered    l;
    const auto value = [&]
    {
        const Place re { .local = l.func.locals.push_back( Local { .type = type } ) };
        const Place im { .local = l.func.locals.push_back( Local { .type = type } ) };
        return Value_place { re, im };
    };
    for( size_t j = 0; j < rows[0].size(); ++j )
    {
        l.in.push_back( value() );
    }
    for( size_t i = 0; i < rows.size(); ++i )
    {
        l.out.push_back( in_place ? l.in[i] : value() );
    }
    const Solution s = solve( field, rows );
    l.body           = lower_circuit( l.func, s.circuit, field, rows, l.in, l.out, type, literals, {} );
    return l;
}

u32 count( const Lowered& l, Rvalue_kind kind, Token_kind op = Token_kind::Unknown )
{
    u32 n = 0;
    for( const Statement& s : l.body )
    {
        const Rvalue& r = l.func.rvalue( s );
        n += r.kind == kind && ( op == Token_kind::Unknown || r.op == op );
    }
    return n;
}

std::vector<Row> dft( Field& field, u32 n )
{
    std::vector<Row> rows( n );
    for( u32 k = 0; k < n; ++k )
    {
        for( u32 j = 0; j < n; ++j )
        {
            rows[k].push_back( field.root( j * k * ( field.n() / n ) ) );
        }
    }
    return rows;
}
} // namespace

TEST_CASE( "lower_circuit_writes_dft4_as_adds", "[egraph][lower]" )
{
    const Type_table types;
    Literal_pool     literals;
    Field            c( Ring_kind::Complex, 4 );
    const Lowered    l = lowered( c, dft( c, 4 ), types.floating( 64 ), literals );

    // 16 real adds; units are swaps and signs, and no product is left.
    CHECK( count( l, Rvalue_kind::Binary ) == 16 );
    CHECK( count( l, Rvalue_kind::Binary, Token_kind::Star ) == 0 );

    Machine   m( l.func, { false, 64 }, literals );
    const f64 x[4][2] = { { 1, 2 }, { 3, -1 }, { 0, 5 }, { -2, 1 } };
    const f64 y[4][2] = { { 2, 7 }, { -1, -8 }, { 0, 7 }, { 3, 2 } };
    for( u32 j = 0; j < 4; ++j )
    {
        m.set( l.in[j].real, { x[j][0] } );
        m.set( l.in[j].imaginary, { x[j][1] } );
    }
    m.run( l.body );
    for( u32 k = 0; k < 4; ++k )
    {
        CHECK( m.get( l.out[k].real ).f == y[k][0] );
        CHECK( m.get( l.out[k].imaginary ).f == y[k][1] );
    }
}

TEST_CASE( "lower_circuit_wraps_an_integer_wht4", "[egraph][lower]" )
{
    const Type_table types;
    Literal_pool     literals;
    Field            z( Ring_kind::Integer, 1, 32 );
    const Element    p = z.one();
    const Element    q = z.minus_one();
    const Lowered    l =
        lowered( z, { { p, p, p, p }, { p, q, p, q }, { p, p, q, q }, { p, q, q, p } }, types.integer( 32, true ), literals );
    CHECK( count( l, Rvalue_kind::Binary ) == 8 );

    Machine   m( l.func, { true, 32 }, literals );
    const u64 x[4] = { 0x7fff'ffff, 1, 0xffff'ffff, 2 };
    for( u32 j = 0; j < 4; ++j )
    {
        m.set( l.in[j].real, { 0, x[j] } );
    }
    m.run( l.body );
    CHECK( m.get( l.out[0].real ).bits == 0x8000'0001 );
    CHECK( m.get( l.out[1].real ).bits == 0x7fff'fffb );
    CHECK( m.get( l.out[2].real ).bits == 0x7fff'ffff );
    CHECK( m.get( l.out[3].real ).bits == 0x8000'0001 );
}

// The debug build checks each lowering on random input; these cover the other constants and rings.
TEST_CASE( "lower_circuit_computes_its_map", "[egraph][lower]" )
{
    const Type_table types;
    Literal_pool     literals;

    SECTION( "dft3 in place, with 2+4 constants" )
    {
        Field         c( Ring_kind::Complex, 3 );
        const Lowered l = lowered( c, dft( c, 3 ), types.floating( 64 ), literals, true );
        CHECK( computes( l.func, l.body, c, dft( c, 3 ), l.in, l.out, literals ) );
    }
    SECTION( "dft8, with (1-i)/sqrt2" )
    {
        Field         c( Ring_kind::Complex, 8 );
        const Lowered l = lowered( c, dft( c, 8 ), types.floating( 64 ), literals );
        CHECK( count( l, Rvalue_kind::Binary, Token_kind::Star ) == 4 );
    }
    SECTION( "a zero row and a negated one" )
    {
        Field         r( Ring_kind::Real, 1 );
        const Element t = r.rational( 3 );
        const Lowered l = lowered(
            r, { { t, r.one() }, { r.zero(), r.zero() }, { r.neg( t ), r.minus_one() } }, types.floating( 64 ), literals
        );
        CHECK( count( l, Rvalue_kind::Unary ) == 1 );
    }
    SECTION( "a wrong body fails" )
    {
        Field                  z( Ring_kind::Integer, 1, 8 );
        const Element          p = z.one();
        const Element          q = z.minus_one();
        const std::vector<Row> rows { { p, p }, { p, q } };
        Lowered                l = lowered( z, rows, types.integer( 8, false ), literals );
        for( const Statement& st : l.body )
        {
            Rvalue& r = l.func.rvalue( st );
            if( r.kind == Rvalue_kind::Binary )
            {
                r.op = r.op == Token_kind::Plus ? Token_kind::Minus : Token_kind::Plus;
                break;
            }
        }
        CHECK( !computes( l.func, l.body, z, rows, l.in, l.out, literals ) );
    }
    SECTION( "gaussian dft4 at 16 bits" )
    {
        Field z( Ring_kind::Integer, 4, 16 );
        CHECK( !lowered( z, dft( z, 4 ), types.integer( 16, false ), literals ).body.empty() );
    }
}
} // namespace keel
#endif // ENABLE_UNIT_TESTS
