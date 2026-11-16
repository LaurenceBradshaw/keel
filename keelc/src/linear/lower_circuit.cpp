// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linear/lower_circuit.h"
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

class Circuit_lowering
{
public:
    Circuit_lowering( Function& func, const Field& field, Type_id type, Literal_pool& literals, Span span )
        : func_( func ),
          field_( field ),
          z_( arithmetic_of( field ) ),
          paired_( paired( field ) ),
          type_( type ),
          literals_( literals ),
          span_( span )
    {
    }

    // `nodes` holds each output's node, or k_zero for zero.
    std::vector<Statement> lower(
        const Circuit&                  circuit,
        const std::vector<u32>&         nodes,
        const std::vector<Value_place>& inputs,
        const std::vector<Value_place>& outputs
    )
    {
        assert( nodes.size() == outputs.size() );
        const std::vector<Op>&                 ops  = circuit.ops();
        const std::vector<std::array<bool, 2>> live = circuit.live_parts();
        std::vector<Value>                     v( ops.size() );
        for( u32 o = 0; o < ops.size(); ++o )
        {
            if( ops[o].kind == Op_kind::Input )
            {
                const Value_place& in = inputs[ops[o].a];
                v[o] = { live[o][0] ? load( in.real ) : Part {}, live[o][1] && paired_ ? load( in.imaginary ) : Part {} };
            }
        }
        for( u32 o = 0; o < ops.size(); ++o )
        {
            if( ( !live[o][0] && !live[o][1] ) || ops[o].kind == Op_kind::Input )
            {
                continue;
            }
            const Value& a = v[ops[o].a];
            if( ops[o].kind == Op_kind::Add )
            {
                const Value& b = v[ops[o].b];
                v[o]           = { live[o][0] ? plus( a.re, b.re ) : Part {}, live[o][1] ? plus( a.im, b.im ) : Part {} };
                continue;
            }
            if( ops[o].kind == Op_kind::Real )
            {
                v[o] = { a.re, {} };
                continue;
            }
            if( ops[o].kind == Op_kind::Conj )
            {
                v[o] = { a.re, negated( a.im ) };
                continue;
            }
            if( ops[o].kind == Op_kind::One )
            {
                v[o] = { constant_part( one() ), {} };
                continue;
            }

            const auto [cr, ci] = z_.integer
                                      ? parts( field_, circuit.exact( ops[o].b ) )
                                      : std::pair<Number, Number> {
                                            { circuit.constant( ops[o].b ).real() }, { circuit.constant( ops[o].b ).imag() }
                                        };
            v[o]                = ops[ops[o].a].kind == Op_kind::One
                                      ? Value { live[o][0] ? constant_part( cr ) : Part {}, live[o][1] ? constant_part( ci ) : Part {} }
                                      : scale( a, cr, ci, live[o] );
        }
        for( size_t i = 0; i < nodes.size(); ++i )
        {
            const Value y = nodes[i] == Circuit::k_zero ? Value {} : v[nodes[i]];
            store( outputs[i].real, y.re );
            if( paired_ && outputs[i].imaginary.local.is_valid() )
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

    // A literal's part; no local is zero.
    Part constant_part( Number k )
    {
        return z_.zero( k ) ? Part {} : emit( use( literal( k ) ) );
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
    // Only the parts `want` says.
    Value scale( Value x, Number cr, Number ci, std::array<bool, 2> want )
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
                want[0] ? times( k, plus( times( sr, x.re ), times( z_.negated( si ), x.im ) ) ) : Part {},
                want[1] ? times( k, plus( times( sr, x.im ), times( si, x.re ) ) ) : Part {},
            };
        }
        return {
            want[0] ? plus( times( cr, x.re ), times( z_.negated( ci ), x.im ) ) : Part {},
            want[1] ? plus( times( cr, x.im ), times( ci, x.re ) ) : Part {},
        };
    }

    static Part negated( Part p )
    {
        return { p.local, !p.negated };
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
        // No place is the constant 1.
        if( !in.real.local.is_valid() )
        {
            x.emplace_back( z.integer ? Number { 0, 1 } : Number { 1.0 }, Number {} );
            continue;
        }
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

// A program's loops as blocks; each leaf lowered by Circuit_lowering on the elements its strides reach.
class Loop_lowering
{
public:
    Loop_lowering(
        Function& func, const Loop_program& program, Type_table& types, Literal_pool& literals, Span span, const Called& called
    )
        : func_( func ),
          program_( program ),
          types_( types ),
          literals_( literals ),
          span_( span ),
          called_( called ),
          real_( types.floating( 64 ) ),
          index_( types.integer( 64, false ) ),
          field_( Ring_kind::Complex, 4 ),
          loops_( program.loops )
    {
        blocks_.emplace_back();
    }

    void lower( Value_place x, Value_place y, std::vector<Statement> prologue )
    {
        blocks_[0].statements = std::move( prologue );
        buffers_              = { x, y };
        for( u64 b = 2; b < program_.buffers.size(); ++b )
        {
            buffers_.push_back( array( program_.buffers[b] ) );
        }
        for( const std::vector<std::complex<f64>>& t : program_.tables )
        {
            std::vector<f64> re, im;
            for( const std::complex<f64>& w : t )
            {
                re.push_back( w.real() );
                im.push_back( w.imag() );
            }
            tables_.push_back( { constant_table( re, real_ ), constant_table( im, real_ ) } );
        }
        for( const std::vector<u64>& t : program_.indices )
        {
            indices_.push_back( constant_table( t, index_ ) );
        }
        run( program_.steps );
        blocks_.back().terminator = { .kind = Terminator_kind::Return, .span = span_ };

        func_.statements = {};
        func_.blocks     = {};
        for( Pending& b : blocks_ )
        {
            const u32 first = static_cast<u32>( func_.statements.size() );
            for( Statement& st : b.statements )
            {
                func_.statements.push_back( std::move( st ) );
            }
            func_.blocks.push_back( Block {
                .first_statement = first,
                .statement_count = static_cast<u32>( b.statements.size() ),
                .terminator      = b.terminator,
            } );
        }
        func_.compact_rvalues();
    }

private:
    struct Pending
    {
        std::vector<Statement> statements;
        Terminator             terminator {};
    };

    void run( const std::vector<Loop_step>& steps )
    {
        for( const Loop_step& s : steps )
        {
            switch( s.kind )
            {
            case Loop_kind::Leaf:
                leaf( s );
                break;
            case Loop_kind::Loop:
                loop(
                    0,
                    s.count,
                    [&]( Local_id t )
                    {
                        loops_[s.loop] = t;
                        run( s.body );
                    }
                );
                break;
            case Loop_kind::Twiddle:
                loop(
                    1,
                    s.count,
                    [&]( Local_id a )
                    {
                        const Local_id ca =
                            emit( binary( Token_kind::Percent, read( times( a, s.c ) ), number_operand( s.M ), index_ ) );
                        const Local_id row = times( a, s.R );
                        loop(
                            1,
                            s.R,
                            [&]( Local_id b )
                            {
                                const Local_id e  = emit( binary(
                                    Token_kind::Percent,
                                    read( emit( binary( Token_kind::Star, read( ca ), read( b ), index_ ) ) ),
                                    number_operand( s.M ),
                                    index_
                                ) );
                                const Local_id at = emit( binary( Token_kind::Plus, read( row ), read( b ), index_ ) );
                                product( buffers_[s.out.buffer], at, tables_[s.table], e, buffers_[s.out.buffer], at );
                            }
                        );
                    }
                );
                break;
            case Loop_kind::Gather:
                loop(
                    0,
                    s.count,
                    [&]( Local_id i )
                    {
                        const Local_id from = emit( use( copy( element( indices_[s.table], i ), index_ ) ) );
                        const Local_id to   = offset( s.out, i );
                        const Local_id at   = offset( s.in, from );
                        const Local_id re   = load( element( buffers_[s.in.buffer].real, at ) );
                        assign( element( buffers_[s.out.buffer].real, to ), use( read( re ) ) );
                        assign(
                            element( buffers_[s.out.buffer].imaginary, to ),
                            s.real ? use( floating( 0 ) )
                            : s.conj
                                ? unary(
                                      Token_kind::Minus, read( load( element( buffers_[s.in.buffer].imaginary, at ) ) ), real_
                                  )
                                : use( read( load( element( buffers_[s.in.buffer].imaginary, at ) ) ) )
                        );
                    }
                );
                break;
            case Loop_kind::Add:
                loop(
                    0,
                    s.count,
                    [&]( Local_id i )
                    {
                        const Local_id a = offset( s.in, i ), b = offset( s.also, i ), o = offset( s.out, i );
                        const auto     sum = [&]( Place Value_place::*part )
                        {
                            return emit( binary(
                                Token_kind::Plus,
                                read( load( element( buffers_[s.in.buffer].*part, a ) ) ),
                                read( load( element( buffers_[s.also.buffer].*part, b ) ) ),
                                real_
                            ) );
                        };
                        const Local_id re = sum( &Value_place::real );
                        const Local_id im =
                            s.real ? load( element( buffers_[s.also.buffer].imaginary, b ) ) : sum( &Value_place::imaginary );
                        assign( element( buffers_[s.out.buffer].real, o ), use( read( re ) ) );
                        assign( element( buffers_[s.out.buffer].imaginary, o ), use( read( im ) ) );
                    }
                );
                break;
            case Loop_kind::Copy:
                loop(
                    0,
                    s.count,
                    [&]( Local_id i )
                    {
                        const Local_id a = offset( s.in, i ), o = offset( s.out, i );
                        for( Place Value_place::*part : { &Value_place::real, &Value_place::imaginary } )
                        {
                            const Local_id v = load( element( buffers_[s.in.buffer].*part, a ) );
                            assign( element( buffers_[s.out.buffer].*part, o ), use( read( v ) ) );
                        }
                    }
                );
                break;
            case Loop_kind::Call:
                call( s );
                break;
            case Loop_kind::Pack:
                loop(
                    0,
                    s.count,
                    [&]( Local_id i )
                    {
                        const Local_id a = offset( s.in, i ), b = offset( s.also, i ), o = offset( s.out, i );
                        const Local_id re = load( element( buffers_[s.in.buffer].real, a ) );
                        const Local_id im = load( element( buffers_[s.also.buffer].real, b ) );
                        assign( element( buffers_[s.out.buffer].real, o ), use( read( re ) ) );
                        assign( element( buffers_[s.out.buffer].imaginary, o ), use( read( im ) ) );
                    }
                );
                break;
            case Loop_kind::Untangle:
                untangle( s );
                break;
            case Loop_kind::Scale:
                loop(
                    0,
                    s.count,
                    [&]( Local_id i )
                    {
                        product(
                            s.real ? Value_place { buffers_[s.in.buffer].real, {} } : buffers_[s.in.buffer],
                            offset( s.in, i ),
                            tables_[s.table],
                            i,
                            buffers_[s.out.buffer],
                            offset( s.out, i )
                        );
                    }
                );
                break;
            }
        }
    }

    // Leaf `s` on the elements its strides reach, each a constant from the loops' part.
    void leaf( const Loop_step& s )
    {
        const Planned&           l = program_.leaves[s.leaf];
        std::vector<Value_place> in, out;
        const Local_id           from = base( s.in ), to = base( s.out );
        for( u32 j = 0; j < s.count; ++j )
        {
            in.push_back( at( buffers_[s.in.buffer], plus( from, s.in.offset + s.in.step * j ) ) );
        }
        for( u32 o = 0; o < l.outputs.size(); ++o )
        {
            out.push_back( at( buffers_[s.out.buffer], plus( to, s.out.offset + s.out.step * o ) ) );
        }
        for( Statement& st : Circuit_lowering( func_, field_, real_, literals_, span_ ).lower( l.circuit, l.outputs, in, out ) )
        {
            blocks_.back().statements.push_back( std::move( st ) );
        }
    }

    // Two real copies' sums a at `out` and b at `also` from Z, the sum of a + i b: b[k] = -i (Z[k] - conj Z[-k]) / 2 and
    // a[k] = Z[k] - i b[k] for k below -k, a and b at -k their conjugates; a = Re Z and b = Im Z where k = -k, and at
    // k past M as at k - M.
    void untangle( const Loop_step& s )
    {
        const Value_place& Z = buffers_[s.in.buffer];
        const Value_place &a = buffers_[s.out.buffer], &b = buffers_[s.also.buffer];
        const auto         put = [&]( const Value_place& to, const Stride& at, Local_id k, Local_id re, Local_id im, bool conj )
        {
            const Local_id o = offset( at, k );
            assign( element( to.real, o ), use( read( re ) ) );
            assign( element( to.imaginary, o ), conj ? unary( Token_kind::Minus, read( im ), real_ ) : use( read( im ) ) );
        };
        const auto own = [&]( u64 k )
        {
            const Local_id at = number( k ), z = offset( s.in, at );
            const Local_id zr = load( element( Z.real, z ) ), zi = load( element( Z.imaginary, z ) ),
                           zero = emit( use( floating( 0 ) ) );
            put( a, s.out, at, zr, zero, false );
            put( b, s.also, at, zi, zero, false );
        };
        const auto pairs = [&]( u64 first, u64 end, bool mirrored )
        {
            loop(
                first,
                end,
                [&]( Local_id k )
                {
                    const Local_id nk = emit( binary( Token_kind::Minus, number_operand( s.M ), read( k ), index_ ) );
                    const Local_id z = offset( s.in, k ), w = offset( s.in, nk );
                    const Local_id zr = load( element( Z.real, z ) ), zi = load( element( Z.imaginary, z ) );
                    const Local_id wr = load( element( Z.real, w ) ), wi = load( element( Z.imaginary, w ) );
                    const auto     f = [&]( Token_kind op, Local_id x, Local_id y )
                    { return emit( binary( op, read( x ), read( y ), real_ ) ); };
                    const auto half = [&]( Local_id x )
                    { return emit( binary( Token_kind::Star, read( x ), floating( 0.5 ), real_ ) ); };
                    const Local_id br = half( f( Token_kind::Plus, zi, wi ) ), bi = half( f( Token_kind::Minus, wr, zr ) );
                    const Local_id ar = f( Token_kind::Plus, zr, bi ), ai = f( Token_kind::Minus, zi, br );
                    put( a, s.out, k, ar, ai, false );
                    put( b, s.also, k, br, bi, false );
                    if( mirrored )
                    {
                        put( a, s.out, nk, ar, ai, true );
                        put( b, s.also, nk, br, bi, true );
                    }
                }
            );
        };
        // k below -k is below (M + 1) / 2, and -k is below m past M - m.
        const u64 m = s.count, M = s.M, end = std::max<u64>( std::min( m, ( M + 1 ) / 2 ), 1 );
        const u64 split = m >= M ? 1 : std::clamp<u64>( M - m + 1, 1, end );
        own( 0 );
        pairs( 1, split, false );
        pairs( split, end, true );
        if( M % 2 == 0 && M / 2 < m )
        {
            own( M / 2 );
        }
        loop(
            M,
            m,
            [&]( Local_id k )
            {
                const Local_id from = emit( binary( Token_kind::Minus, read( k ), number_operand( M ), index_ ) );
                for( const auto& [to, at] : { std::pair { &a, &s.out }, { &b, &s.also } } )
                {
                    const Local_id i = offset( *at, from );
                    put( *to, *at, k, load( element( to->real, i ) ), load( element( to->imaginary, i ) ), false );
                }
            }
        );
    }

    // The callee on the addresses of the strides' first elements, cast to its parameters.
    void call( const Loop_step& s )
    {
        const Loop_call& callee = program_.calls[s.leaf];
        assert( buffers_[s.in.buffer].imaginary.local.is_valid() && "a call reads a real buffer" );
        called_( callee.kernel, callee.program );
        const Local_id       from = plus( base( s.in ), s.in.offset ), into = plus( base( s.out ), s.out.offset );
        std::vector<Operand> arguments;
        for( u32 a = 0; a < 4; ++a )
        {
            const Value_place& buffer = buffers_[a < 2 ? s.in.buffer : s.out.buffer];
            const Place&       part   = ( a % 2 == 0 ) != s.conj ? buffer.real : buffer.imaginary;
            const Local_id     first  = emit( address_of(
                element( part, a < 2 ? from : into ),
                types_.pointer_to( real_, a < 2 ),
                a < 2 ? Address_purpose::Read : Address_purpose::Borrow,
                span_
            ) );
            arguments.push_back( read( emit( cast_to( read( first ), kernel_parameter( types_, callee.kernel, a ) ) ) ) );
        }
        Rvalue c = keel::call(
            {}, std::move( arguments ), types_.builtin( Type_kind::Void ), kernel_arguments( types_, callee.kernel )
        );
        c.planned = true;
        emit( std::move( c ) );
    }

    // out[o] = x[i] w[e], complex; x real when it has no imaginary part.
    void product( Value_place x, Local_id i, Value_place w, Local_id e, Value_place out, Local_id o )
    {
        const Local_id xr = load( element( x.real, i ) );
        const Local_id wr = load( element( w.real, e ) ), wi = load( element( w.imaginary, e ) );
        const auto     mul = [&]( Local_id a, Local_id b )
        { return emit( binary( Token_kind::Star, read( a ), read( b ), real_ ) ); };
        if( !x.imaginary.local.is_valid() )
        {
            assign( element( out.real, o ), use( read( mul( xr, wr ) ) ) );
            assign( element( out.imaginary, o ), use( read( mul( xr, wi ) ) ) );
            return;
        }
        const Local_id xi = load( element( x.imaginary, i ) );
        const Local_id re = emit( binary( Token_kind::Minus, read( mul( xr, wr ) ), read( mul( xi, wi ) ), real_ ) );
        const Local_id im = emit( binary( Token_kind::Plus, read( mul( xr, wi ) ), read( mul( xi, wr ) ), real_ ) );
        assign( element( out.real, o ), use( read( re ) ) );
        assign( element( out.imaginary, o ), use( read( im ) ) );
    }

    // `body` with t from `first` below `count`: a head testing t, the body, t + 1 back to the head.
    template <class Body>
    void loop( u64 first, u64 count, Body body )
    {
        const Local_id t = func_.locals.push_back( Local { .type = index_, .span = span_ } );
        assign( Place { .local = t }, use( number_operand( first ) ) );
        const u32      head = goto_next();
        const Local_id more =
            emit( binary( Token_kind::Less, read( t ), number_operand( count ), types_.builtin( Type_kind::Bool ) ) );
        goto_next();
        body( t );
        assign( Place { .local = t }, binary( Token_kind::Plus, read( t ), number_operand( 1 ), index_ ) );
        blocks_.back().terminator = { .kind = Terminator_kind::Goto, .span = span_, .targets = { Block_id { head } } };
        blocks_.emplace_back();
        blocks_[head].terminator = {
            .kind      = Terminator_kind::Branch,
            .span      = span_,
            .condition = read( more ),
            .targets   = { Block_id { head + 1 }, Block_id { static_cast<u32>( blocks_.size() - 1 ) } },
        };
    }

    // Ends the current block with a goto to a new one; the new one's index.
    u32 goto_next()
    {
        const u32 to              = static_cast<u32>( blocks_.size() );
        blocks_.back().terminator = { .kind = Terminator_kind::Goto, .span = span_, .targets = { Block_id { to } } };
        blocks_.emplace_back();
        return to;
    }

    // The loops' part of a stride's element.
    Local_id base( const Stride& a )
    {
        Local_id acc = number( 0 );
        for( const auto& [l, coefficient] : a.loops )
        {
            acc = emit( binary( Token_kind::Plus, read( acc ), read( times( loops_[l], coefficient ) ), index_ ) );
        }
        return acc;
    }

    Local_id offset( const Stride& a, Local_id i )
    {
        return emit( binary( Token_kind::Plus, read( plus( base( a ), a.offset ) ), read( times( i, a.step ) ), index_ ) );
    }

    Local_id times( Local_id a, u64 k )
    {
        return emit( binary( Token_kind::Star, read( a ), number_operand( k ), index_ ) );
    }

    Local_id plus( Local_id a, u64 k )
    {
        return emit( binary( Token_kind::Plus, read( a ), number_operand( k ), index_ ) );
    }

    Value_place array( u64 size )
    {
        const Type_id t = types_.array_of( real_, size );
        return {
            Place { .local = func_.locals.push_back( Local { .type = t, .span = span_ } ) },
            Place { .local = func_.locals.push_back( Local { .type = t, .span = span_ } ) }
        };
    }

    // The array behind a pointer to read-only `values`, each of `type`.
    template <class T>
    Place constant_table( const std::vector<T>& values, Type_id type )
    {
        const Type_id  t = types_.pointer_to( types_.array_of( type, values.size() ), true );
        const Local_id p = emit( constant_array(
            literals_.intern_string( std::string( reinterpret_cast<const char*>( values.data() ), values.size() * sizeof( T ) )
            ),
            t
        ) );
        return projected( func_, Place { .local = p }, Projection { .kind = Projection_kind::Deref } );
    }

    // No imaginary part for a real buffer's.
    Value_place at( Value_place buffer, Local_id i )
    {
        return { element( buffer.real, i ), buffer.imaginary.local.is_valid() ? element( buffer.imaginary, i ) : Place {} };
    }

    Place element( Place array, Local_id i )
    {
        return projected( func_, array, Projection { .kind = Projection_kind::Index, .index = i } );
    }

    Local_id load( const Place& p )
    {
        return emit( use( copy( p, real_ ) ) );
    }

    Local_id number( u64 k )
    {
        return emit( use( number_operand( k ) ) );
    }

    Operand number_operand( u64 k ) const
    {
        return constant( literals_.add_integer( k ), index_ );
    }

    Operand floating( f64 v ) const
    {
        return constant( literals_.add_float( v, static_cast<f32>( v ) ), real_ );
    }

    Operand read( Local_id l ) const
    {
        return copy( Place { .local = l }, func_.locals[l].type );
    }

    Local_id emit( Rvalue value )
    {
        const Local_id t = func_.locals.push_back( Local { .type = value.type, .span = span_ } );
        assign( Place { .local = t }, std::move( value ) );
        return t;
    }

    void assign( Place place, Rvalue value )
    {
        blocks_.back().statements.push_back(
            Statement { .span = span_, .place = place, .value = func_.add_rvalue( std::move( value ) ) }
        );
    }

    Function&                func_;
    const Loop_program&      program_;
    Type_table&              types_;
    Literal_pool&            literals_;
    Span                     span_;
    const Called&            called_;
    Type_id                  real_;
    Type_id                  index_;
    Field                    field_;
    std::vector<Local_id>    loops_;
    std::vector<Pending>     blocks_;
    std::vector<Value_place> buffers_;
    std::vector<Value_place> tables_;
    std::vector<Place>       indices_;
};
} // namespace

std::vector<Type_id> kernel_arguments( Type_table& types, const Kernel& k )
{
    return { types.size( k.n ), types.size( k.m ), types.size( k.M ), types.size( k.c ) };
}

Type_id kernel_parameter( Type_table& types, const Kernel& k, u32 p )
{
    return types.pointer_to( types.array_of( types.floating( 64 ), p < 2 ? k.n : k.m ), p < 2 );
}

std::vector<Statement> lower_circuit(
    Function&                       func,
    const Circuit&                  circuit,
    const Field&                    field,
    const std::vector<u32>&         nodes,
    const std::vector<Value_place>& inputs,
    const std::vector<Value_place>& outputs,
    Type_id                         type,
    Literal_pool&                   literals,
    Span                            span,
    const std::vector<Row>&         rows
)
{
    std::vector<Statement> body =
        Circuit_lowering( func, field, type, literals, span ).lower( circuit, nodes, inputs, outputs );
    assert(
        ( rows.empty() || computes( func, body, field, rows, inputs, outputs, literals ) ) &&
        "a lowered circuit disagrees with its map"
    );
    return body;
}

void lower_program(
    Function&              func,
    const Loop_program&    program,
    Value_place            x,
    Value_place            y,
    std::vector<Statement> prologue,
    Type_table&            types,
    Literal_pool&          literals,
    Span                   span,
    const Called&          called
)
{
    Loop_lowering( func, program, types, literals, span, called ).lower( x, y, std::move( prologue ) );
}
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include "linear/solve.h"

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
    l.body = lower_circuit( l.func, s.circuit, field, s.circuit.nodes( rows ), l.in, l.out, type, literals, {}, rows );
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

TEST_CASE( "lower_circuit_writes_dft4_as_adds", "[linear][lower]" )
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

TEST_CASE( "lower_circuit_wraps_an_integer_wht4", "[linear][lower]" )
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
TEST_CASE( "lower_circuit_computes_its_map", "[linear][lower]" )
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
