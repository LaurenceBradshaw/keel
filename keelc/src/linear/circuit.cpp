// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linear/circuit.h"
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <numbers>
#include <span>
#include <sstream>

namespace keel
{
namespace
{
constexpr std::complex<f64> k_real_units[]    = { { 1, 0 }, { -1, 0 } };
constexpr std::complex<f64> k_complex_units[] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };

// A real constant's orbit stays real, so a real node scaled stays real.
std::span<const std::complex<f64>> float_units( std::complex<f64> c )
{
    if( std::abs( c.imag() ) < 1e-12 )
    {
        return k_real_units;
    }
    return k_complex_units;
}

bool is_float_unit( std::complex<f64> c )
{
    return std::ranges::any_of( k_complex_units, [&]( std::complex<f64> u ) { return std::abs( c - u ) < 1e-12; } );
}

// One multiply by a part other than 0 and +-1.
u32 part_mul( f64 v )
{
    return std::abs( v ) > 1e-9 && std::abs( std::abs( v ) - 1 ) > 1e-9 ? 1 : 0;
}

// The live parts of `c` times a value, as lower_circuit computes them: re = x a - y b, im = x b + y a.
Cost scaled_parts( std::complex<f64> c, bool real, bool re, bool im )
{
    if( re && im && !real )
    {
        return scale_cost( c, false );
    }
    const f64 x = c.real(), y = c.imag();
    if( real )
    {
        return { 0, ( re ? part_mul( x ) : 0 ) + ( im ? part_mul( y ) : 0 ) };
    }
    if( std::abs( x ) < 1e-9 || std::abs( y ) < 1e-9 )
    {
        return { 0, part_mul( x ) + part_mul( y ) };
    }
    if( std::abs( std::abs( x ) - std::abs( y ) ) < 1e-9 )
    {
        return { 1, part_mul( x ) };
    }
    return { 1, part_mul( x ) + part_mul( y ) };
}

// Cleans float noise so units and real or imaginary constants are recognised.
std::complex<f64> snap( std::complex<f64> c )
{
    const auto fix = []( f64 v ) {
        return std::abs( v ) < 1e-12 ? 0.0 : std::abs( std::abs( v ) - 1 ) < 1e-12 ? std::round( v ) : v;
    };
    return { fix( c.real() ), fix( c.imag() ) };
}

// Equal to nine places, -0 as 0.
std::pair<f64, f64> key( std::complex<f64> c )
{
    return { std::round( c.real() * 1e9 ) + 0.0, std::round( c.imag() * 1e9 ) + 0.0 };
}

// An op as saved() writes it.
std::string op_line( const Op& op )
{
    switch( op.kind )
    {
    case Op_kind::Input:
        return "x " + std::to_string( op.a ) + '\n';
    case Op_kind::Add:
        return "a " + std::to_string( op.a ) + ' ' + std::to_string( op.b ) + '\n';
    case Op_kind::Scale:
        return "m " + std::to_string( op.a ) + ' ' + std::to_string( op.b ) + '\n';
    case Op_kind::Real:
        return "r " + std::to_string( op.a ) + '\n';
    case Op_kind::Conj:
        return "c " + std::to_string( op.a ) + '\n';
    case Op_kind::One:
        return "1\n";
    case Op_kind::Result:
        break;
    }
    return {};
}
} // namespace

Circuit::Circuit( bool real )
    : real_( real )
{
}

Circuit::Circuit( Field& integers )
    : real_( integers.n() <= 2 ),
      integers_( &integers )
{
    assert( integers.kind() == Ring_kind::Integer );
}

u32 Circuit::put( Op op )
{
    assert( op.a < 1u << 30 && op.b < 1u << 31 );
    const u64 packed           = u64( op.a ) << 34 | u64( op.b ) << 3 | u64( op.kind );
    const auto [found, is_new] = memo_.try_emplace( packed, static_cast<u32>( ops_.size() ) );
    if( is_new )
    {
        ops_.push_back( op );
        const bool real = op.kind == Op_kind::Input   ? real_
                          : op.kind == Op_kind::Add   ? real_ops_[op.a] && real_ops_[op.b]
                          : op.kind == Op_kind::Scale ? real_ops_[op.a] && std::abs( constants_[op.b].value.imag() ) < 1e-12
                          : op.kind == Op_kind::Conj  ? real_ops_[op.a]
                                                      : op.kind == Op_kind::Real || op.kind == Op_kind::One;
        real_ops_.push_back( real );
        const bool imaginary = op.kind == Op_kind::Add ? imaginary_ops_[op.a] && imaginary_ops_[op.b]
                               : op.kind == Op_kind::Scale
                                   ? ( real_ops_[op.a] && std::abs( constants_[op.b].value.real() ) < 1e-12 ) ||
                                         ( imaginary_ops_[op.a] && std::abs( constants_[op.b].value.imag() ) < 1e-12 )
                               : op.kind == Op_kind::Conj ? imaginary_ops_[op.a]
                                                          : false;
        imaginary_ops_.push_back( imaginary );
    }
    return found->second;
}

u32 Circuit::input( u32 j )
{
    return put( { Op_kind::Input, j, 0 } );
}

u32 Circuit::add( u32 a, u32 b )
{
    if( is_constant( a ) && is_constant( b ) )
    {
        return scale_by( one(), plus( value_of( a ), value_of( b ) ) );
    }
    return put( { Op_kind::Add, std::min( a, b ), std::max( a, b ) } );
}

u32 Circuit::scale( u32 a, std::complex<f64> c )
{
    return scale_by( a, make( c ) );
}

u32 Circuit::scale( u32 a, Element c )
{
    return scale_by( a, make( c ) );
}

u32 Circuit::real_part( u32 a )
{
    if( !real_ops_[a] && !integers_ && is_constant( a ) )
    {
        return scale( one(), value_of( a ).value.real() );
    }
    return real_ops_[a] ? a : put( { Op_kind::Real, a, 0 } );
}

u32 Circuit::conj( u32 a )
{
    if( !real_ops_[a] && !integers_ && is_constant( a ) )
    {
        return scale( one(), std::conj( value_of( a ).value ) );
    }
    return real_ops_[a] ? a : ops_[a].kind == Op_kind::Conj ? ops_[a].a : put( { Op_kind::Conj, a, 0 } );
}

u32 Circuit::one()
{
    return put( { Op_kind::One, 0, 0 } );
}

bool Circuit::is_constant( u32 a ) const
{
    return ops_[a].kind == Op_kind::One || ( ops_[a].kind == Op_kind::Scale && ops_[ops_[a].a].kind == Op_kind::One );
}

Circuit::Constant Circuit::value_of( u32 a ) const
{
    if( ops_[a].kind == Op_kind::Scale )
    {
        return constants_[ops_[a].b];
    }
    return integers_ ? make( integers_->one() ) : make( 1.0 );
}

u32 Circuit::call( Call call )
{
    const auto [found, is_new] =
        call_ids_.try_emplace( { call.callee, call.conjugate, call.arguments }, static_cast<u32>( calls_.size() ) );
    if( is_new )
    {
        calls_.push_back( std::move( call ) );
    }
    return found->second;
}

u32 Circuit::result( u32 call, u32 k )
{
    return put( { Op_kind::Result, call, k } );
}

// `sub`'s call `c` made here, its arguments mapped through `nodes`.
u32 Circuit::call_from( const Circuit& sub, u32 c, const std::vector<u32>& nodes, bool conjugate )
{
    Call made = sub.calls_[c];
    for( u32& a : made.arguments )
    {
        a = a == k_zero ? k_zero : nodes[a];
    }
    made.conjugate = made.conjugate != conjugate;
    return call( std::move( made ) );
}

Circuit::Constant Circuit::make( std::complex<f64> c ) const
{
    assert( !integers_ );
    return { snap( c ), {} };
}

Circuit::Constant Circuit::make( Element c ) const
{
    assert( integers_ );
    return { integers_->value( c ), c };
}

Circuit::Constant Circuit::times( Constant x, Constant y ) const
{
    return integers_ ? make( integers_->mul( x.exact, y.exact ) ) : make( x.value * y.value );
}

Circuit::Constant Circuit::plus( Constant x, Constant y ) const
{
    return integers_ ? make( integers_->add( x.exact, y.exact ) ) : make( x.value + y.value );
}

bool Circuit::is_unit( Constant c ) const
{
    if( integers_ )
    {
        return integers_->is_unit( c.exact );
    }
    return is_float_unit( c.value );
}

u32 Circuit::scale_node( u32 a, Constant c )
{
    const std::pair<f64, f64> k = integers_ ? std::pair<f64, f64> { static_cast<f64>( c.exact.v ), 0 } : key( c.value );
    const auto [found, is_new]  = constant_ids_.try_emplace( k, static_cast<u32>( constants_.size() ) );
    if( is_new )
    {
        constants_.push_back( c );
    }
    return put( { Op_kind::Scale, a, found->second } );
}

u32 Circuit::scale_by( u32 a, Constant c )
{
    // A constant times c is one constant, unit or not.
    if( is_constant( a ) )
    {
        c = times( c, value_of( a ) );
        return ( integers_ ? c.exact == integers_->one() : c.value == 1.0 ) ? one() : scale_node( one(), c );
    }
    // Units fold into the constant, which is kept as unit times the least of its orbit, so -cx reuses cx.
    if( ops_[a].kind == Op_kind::Scale && is_unit( constants_[ops_[a].b] ) )
    {
        c = times( c, constants_[ops_[a].b] );
        a = ops_[a].a;
    }
    // c conj(x) = conj(conj(c) x), so conjugate scalings share one multiply.
    if( !integers_ && ops_[a].kind == Op_kind::Conj )
    {
        return conj( scale_by( ops_[a].a, make( std::conj( c.value ) ) ) );
    }

    if( integers_ )
    {
        if( integers_->is_unit( c.exact ) )
        {
            return c.exact == integers_->one() ? a : scale_node( a, c );
        }
        const auto [rep, u] = integers_->orbit( c.exact );
        const u32 inner     = scale_node( a, make( rep ) );
        return u == integers_->one() ? inner : scale_node( inner, make( integers_->inv( u ) ) );
    }

    if( is_unit( c ) )
    {
        return c.value == 1.0 ? a : scale_node( a, c );
    }
    const auto        units = float_units( c.value );
    std::complex<f64> v     = units[0];
    for( const std::complex<f64> u : units )
    {
        v = key( u * c.value ) < key( v * c.value ) ? u : v;
    }
    const u32 inner = scale_node( a, make( v * c.value ) );
    return v == 1.0 ? inner : scale_node( inner, make( 1.0 / v ) );
}

std::vector<u32> Circuit::paste( const Circuit& sub, const std::vector<u32>& inputs, bool conjugate )
{
    assert( integers_ == sub.integers_ && !( conjugate && integers_ ) );
    std::vector<u32> m;
    std::vector<u32> made( sub.calls_.size(), k_zero );
    m.reserve( sub.ops_.size() );
    for( const Op& op : sub.ops_ )
    {
        switch( op.kind )
        {
        case Op_kind::Result:
            if( made[op.a] == k_zero )
            {
                made[op.a] = call_from( sub, op.a, m, conjugate );
            }
            m.push_back( result( made[op.a], op.b ) );
            break;
        case Op_kind::Input:
            m.push_back( inputs[op.a] );
            break;
        case Op_kind::One:
            m.push_back( one() );
            break;
        case Op_kind::Add:
            m.push_back( add( m[op.a], m[op.b] ) );
            break;
        case Op_kind::Scale:
            m.push_back( scale_by( m[op.a], conjugate ? make( std::conj( sub.constants_[op.b].value ) ) : sub.constants_[op.b] )
            );
            break;
        case Op_kind::Real:
            m.push_back( real_part( m[op.a] ) );
            break;
        case Op_kind::Conj:
            m.push_back( conj( m[op.a] ) );
            break;
        }
    }
    return m;
}

std::vector<bool> Circuit::live() const
{
    std::vector<bool> seen( ops_.size(), false );
    std::vector<u32>  stack;
    for( const auto& [row, o] : outputs )
    {
        stack.push_back( o );
    }
    while( !stack.empty() )
    {
        const u32 o = stack.back();
        stack.pop_back();
        if( seen[o] )
        {
            continue;
        }
        seen[o] = true;
        if( ops_[o].kind == Op_kind::Add )
        {
            stack.push_back( ops_[o].a );
            stack.push_back( ops_[o].b );
        }
        else if( ops_[o].kind == Op_kind::Result )
        {
            for( const u32 a : calls_[ops_[o].a].arguments )
            {
                if( a != k_zero )
                {
                    stack.push_back( a );
                }
            }
        }
        else if( ops_[o].kind != Op_kind::Input && ops_[o].kind != Op_kind::One )
        {
            stack.push_back( ops_[o].a );
        }
    }
    return seen;
}

std::vector<std::array<bool, 2>> Circuit::live_parts() const
{
    std::vector<std::array<bool, 2>> live( ops_.size(), { false, false } );
    const auto                       need = [&]( u32 a, bool re, bool im )
    {
        live[a][0] = live[a][0] || re;
        live[a][1] = live[a][1] || ( im && !real_ops_[a] );
    };
    for( const auto& [row, o] : outputs )
    {
        need( o, true, true );
    }
    for( u32 o = static_cast<u32>( ops_.size() ); o-- > 0; )
    {
        const auto [re, im] = live[o];
        const Op& op        = ops_[o];
        if( op.kind == Op_kind::Add )
        {
            need( op.a, re, im );
            need( op.b, re, im );
        }
        else if( op.kind == Op_kind::Scale )
        {
            const std::complex<f64> c = constants_[op.b].value;
            const bool              x = std::abs( c.real() ) > 1e-12, y = std::abs( c.imag() ) > 1e-12;
            need( op.a, ( re && x ) || ( im && y ), ( re && y ) || ( im && x ) );
        }
        else if( op.kind == Op_kind::Real )
        {
            need( op.a, re, false );
        }
        else if( op.kind == Op_kind::Conj )
        {
            need( op.a, re, im );
        }
        // The callee reads every argument whole.
        else if( op.kind == Op_kind::Result && ( re || im ) )
        {
            for( const u32 a : calls_[op.a].arguments )
            {
                if( a != k_zero )
                {
                    need( a, true, true );
                }
            }
        }
    }
    return live;
}

std::vector<u32> Circuit::uses( const std::vector<bool>& lv ) const
{
    std::vector<u32>  users( ops_.size(), 0 );
    std::vector<bool> counted( calls_.size(), false );
    for( u32 o = 0; o < ops_.size(); ++o )
    {
        if( !lv[o] || ops_[o].kind == Op_kind::Input || ops_[o].kind == Op_kind::One )
        {
            continue;
        }
        if( ops_[o].kind == Op_kind::Result )
        {
            for( const u32 a : calls_[ops_[o].a].arguments )
            {
                if( a != k_zero && !counted[ops_[o].a] )
                {
                    ++users[a];
                }
            }
            counted[ops_[o].a] = true;
            continue;
        }
        ++users[ops_[o].a];
        users[ops_[o].b] += ops_[o].kind == Op_kind::Add;
    }
    for( const auto& [row, o] : outputs )
    {
        ++users[o];
    }
    return users;
}

Circuit Circuit::pruned() const
{
    const std::vector<bool> lv    = live();
    const std::vector<u32>  users = uses( lv );

    Circuit          c = integers_ ? Circuit( *integers_ ) : Circuit( real_ );
    std::vector<u32> m( ops_.size(), 0 );
    std::vector<u32> made( calls_.size(), k_zero );
    bool             folded = false;
    for( u32 o = 0; o < ops_.size(); ++o )
    {
        if( !lv[o] )
        {
            continue;
        }

        const Op& op = ops_[o];
        if( op.kind == Op_kind::Input )
        {
            m[o] = c.input( op.a );
        }
        else if( op.kind == Op_kind::One )
        {
            m[o] = c.one();
        }
        else if( op.kind == Op_kind::Add )
        {
            m[o] = c.add( m[op.a], m[op.b] );
        }
        else if( op.kind == Op_kind::Result )
        {
            if( made[op.a] == k_zero )
            {
                made[op.a] = c.call_from( *this, op.a, m, false );
            }
            m[o] = c.result( made[op.a], op.b );
        }
        else if( op.kind == Op_kind::Real )
        {
            m[o] = c.real_part( m[op.a] );
        }
        else if( op.kind == Op_kind::Conj )
        {
            m[o] = c.conj( m[op.a] );
        }
        else if( const Op& inner = ops_[op.a]; inner.kind == Op_kind::Scale && users[op.a] == 1 &&
                                               !is_unit( constants_[op.b] ) && !is_unit( constants_[inner.b] ) )
        {
            m[o]   = c.scale_by( m[inner.a], times( constants_[inner.b], constants_[op.b] ) );
            folded = true;
        }
        else
        {
            m[o] = c.scale_by( m[op.a], constants_[op.b] );
        }
    }
    for( const auto& [row, o] : outputs )
    {
        c.outputs.emplace( row, m[o] );
    }
    return folded ? c.pruned() : c;
}

Circuit Circuit::bound( u32 j ) const
{
    Circuit          c = integers_ ? Circuit( *integers_ ) : Circuit( real_ );
    std::vector<u32> inputs;
    for( const Op& op : ops_ )
    {
        if( op.kind == Op_kind::Input )
        {
            inputs.resize( std::max<size_t>( inputs.size(), op.a + 1 ) );
            inputs[op.a] = op.a == j ? c.one() : c.input( op.a );
        }
    }
    const std::vector<u32> m = c.paste( *this, inputs );
    for( const auto& [row, o] : outputs )
    {
        c.outputs.emplace( row, m[o] );
    }
    return c;
}

// Complex: real or imaginary 0+2, |Re| = |Im| or a part +-1 2+2, else 2+4. Real: a multiply a part. A unit is free.
Cost scale_cost( std::complex<f64> c, bool real, bool unit )
{
    c = snap( c );
    if( unit || is_float_unit( c ) )
    {
        return {};
    }
    const f64 x = std::abs( c.real() );
    const f64 y = std::abs( c.imag() );
    if( real )
    {
        return { 0, part_mul( x ) + part_mul( y ) };
    }
    if( x < 1e-9 || y < 1e-9 )
    {
        return { 0, 2 };
    }
    if( std::abs( x - y ) < 1e-9 || std::abs( x - 1 ) < 1e-9 || std::abs( y - 1 ) < 1e-9 )
    {
        return { 2, 2 };
    }
    return { 2, 4 };
}

Cost Circuit::cost( bool fma ) const
{
    const std::vector<std::array<bool, 2>> parts = live_parts();
    const std::vector<bool>                lv    = live();
    fma                                          = fma && integers_ == nullptr;
    const std::vector<u32> users                 = fma ? uses( lv ) : std::vector<u32> {};

    // A constant's lowering ends in its multiplies when a part is 0 or |Re| = |Im| (k(+-1 +-i)), and
    // otherwise ends in adds that each take one of them.
    const auto ends_in_muls = [&]( u32 n, const Constant& c )
    {
        const f64 x = std::abs( c.value.real() );
        const f64 y = std::abs( c.value.imag() );
        return real_ops_[ops_[n].a] || x < 1e-9 || y < 1e-9 || ( std::abs( x - y ) < 1e-9 && std::abs( x - 1 ) >= 1e-9 );
    };
    // The add after a multiply takes it when that is its only user, through a unit (a sign or swap).
    const auto feeds = [&]( u32 n )
    {
        if( ops_[n].kind == Op_kind::Scale && users[n] == 1 && is_unit( constants_[ops_[n].b] ) )
        {
            n = ops_[n].a;
        }
        return ops_[n].kind == Op_kind::Scale && !is_constant( n ) && users[n] == 1 && !is_unit( constants_[ops_[n].b] ) &&
               ends_in_muls( n, constants_[ops_[n].b] );
    };

    Cost              total;
    u32               fused = 0;
    std::vector<bool> called( calls_.size(), false );
    for( u32 o = 0; o < ops_.size(); ++o )
    {
        const Op& op = ops_[o];
        if( lv[o] && op.kind == Op_kind::Result && !called[op.a] )
        {
            called[op.a] = true;
            total        = total + calls_[op.a].cost;
        }
        if( !lv[o] || op.kind == Op_kind::Input || op.kind == Op_kind::Real || op.kind == Op_kind::Conj ||
            op.kind == Op_kind::Result || is_constant( o ) )
        {
            continue;
        }
        const auto [re, im] = parts[o];
        if( op.kind == Op_kind::Add )
        {
            // A part only one side has is a copy.
            const u32 adds = ( re && !imaginary_ops_[op.a] && !imaginary_ops_[op.b] ? 1 : 0 ) +
                             ( im && !real_ops_[op.a] && !real_ops_[op.b] ? 1 : 0 );
            total.adds += adds;
            if( fma && ( feeds( op.a ) || feeds( op.b ) ) )
            {
                fused += adds;
            }
            continue;
        }

        const Constant& c = constants_[op.b];
        if( is_unit( c ) )
        {
            continue;
        }
        const Cost s = scaled_parts( c.value, real_ops_[op.a], re, im );
        total.adds += s.adds;
        total.muls += s.muls;
        if( fma && re && im && !ends_in_muls( o, c ) )
        {
            fused += 2;
        }
    }
    total.muls -= fused;
    return total;
}

std::vector<u32> Circuit::nodes( const std::vector<Row>& rows ) const
{
    std::vector<u32> out;
    for( const Row& row : rows )
    {
        const auto found = outputs.find( row );
        out.push_back( found == outputs.end() ? k_zero : found->second );
    }
    return out;
}

std::vector<std::complex<f64>> Circuit::evaluate( const std::vector<std::complex<f64>>& inputs ) const
{
    std::vector<std::complex<f64>>              v;
    std::vector<std::vector<std::complex<f64>>> results( calls_.size() );
    v.reserve( ops_.size() );
    for( const Op& op : ops_ )
    {
        switch( op.kind )
        {
        case Op_kind::Input:
            v.push_back( inputs[op.a] );
            break;
        case Op_kind::Result:
            if( results[op.a].empty() )
            {
                const Call& c = calls_[op.a];
                const u64   n = c.arguments.size();
                results[op.a].assign( n, 0 );
                for( u64 k = 0; k < n; ++k )
                {
                    for( u64 j = 0; j < n; ++j )
                    {
                        const std::complex<f64> x = c.arguments[j] == k_zero ? 0 : v[c.arguments[j]];
                        const f64 t = -2.0 * std::numbers::pi * static_cast<f64>( j * k % n ) / static_cast<f64>( n );
                        results[op.a][k] += ( c.conjugate ? std::conj( x ) : x ) * std::polar( 1.0, t );
                    }
                    results[op.a][k] = c.conjugate ? std::conj( results[op.a][k] ) : results[op.a][k];
                }
            }
            v.push_back( results[op.a][op.b] );
            break;
        case Op_kind::Add:
            v.push_back( v[op.a] + v[op.b] );
            break;
        case Op_kind::Scale:
            v.push_back( constants_[op.b].value * v[op.a] );
            break;
        case Op_kind::Real:
            v.push_back( v[op.a].real() );
            break;
        case Op_kind::Conj:
            v.push_back( std::conj( v[op.a] ) );
            break;
        case Op_kind::One:
            v.push_back( 1 );
            break;
        }
    }
    return v;
}

std::string Circuit::text() const
{
    std::ostringstream out;
    out.precision( 17 );
    for( u32 o = 0; o < ops_.size(); ++o )
    {
        const Op& op = ops_[o];
        switch( op.kind )
        {
        case Op_kind::Input:
            out << o << " x " << op.a << '\n';
            break;
        case Op_kind::Add:
            out << o << " add " << op.a << ' ' << op.b << '\n';
            break;
        case Op_kind::Scale:
            out << o << " mul " << op.a << ' ' << constants_[op.b].value.real() << ' ' << constants_[op.b].value.imag() << '\n';
            break;
        case Op_kind::Real:
            out << o << " re " << op.a << '\n';
            break;
        case Op_kind::Conj:
            out << o << " conj " << op.a << '\n';
            break;
        case Op_kind::One:
            out << o << " one\n";
            break;
        case Op_kind::Result:
            out << o << " result " << op.a << ' ' << op.b << '\n';
            break;
        }
    }
    for( u32 c = 0; c < calls_.size(); ++c )
    {
        out << "call " << c << " of " << calls_[c].callee << ( calls_[c].conjugate ? " conjugated" : "" );
        for( const u32 a : calls_[c].arguments )
        {
            out << ' ' << ( a == k_zero ? std::string( "0" ) : std::to_string( a ) );
        }
        out << '\n';
    }
    for( const auto& [row, o] : outputs )
    {
        out << "y " << o;
        for( const Element e : row )
        {
            out << ' ' << e.v;
        }
        out << '\n';
    }
    return out.str();
}

std::string Circuit::saved( const std::vector<Row>& rows ) const
{
    assert( calls_.empty() && "a call's callee is this build's" );
    std::string out    = std::to_string( ops_.size() ) + ' ' + std::to_string( constants_.size() ) + '\n';
    const auto  number = [&]( f64 v )
    {
        char       buffer[32];
        const auto end = std::to_chars( buffer, buffer + sizeof buffer, v ).ptr;
        out += ' ';
        out.append( buffer, end );
    };
    for( const Constant& c : constants_ )
    {
        out += 'c';
        if( integers_ )
        {
            for( u32 k = 0; k < integers_->degree(); ++k )
            {
                out += ' ' + std::to_string( integers_->coefficient( c.exact, k ).first );
            }
        }
        else
        {
            number( c.value.real() );
            number( c.value.imag() );
        }
        out += '\n';
    }
    for( const Op& op : ops_ )
    {
        out += op_line( op );
    }
    for( size_t i = 0; i < rows.size(); ++i )
    {
        if( const auto found = outputs.find( rows[i] ); found != outputs.end() )
        {
            out += "y " + std::to_string( i ) + ' ' + std::to_string( found->second ) + '\n';
        }
    }
    return out + "end\n";
}

std::optional<Circuit> Circuit::load( std::string_view text, Field& field, const std::vector<Row>& rows )
{
    Circuit            c = field.kind() == Ring_kind::Integer ? Circuit( field ) : Circuit( field.kind() == Ring_kind::Real );
    std::istringstream in { std::string( text ) };
    const auto         word = [&]
    {
        std::string w;
        in >> w;
        return w;
    };
    const auto whole = [&]( u64 below ) -> std::optional<u32>
    {
        const std::string w     = word();
        u64               v     = 0;
        const auto [end, error] = std::from_chars( w.data(), w.data() + w.size(), v );
        if( w.empty() || error != std::errc {} || end != w.data() + w.size() || v >= below )
        {
            return std::nullopt;
        }
        return static_cast<u32>( v );
    };
    const auto real = [&]() -> std::optional<f64>
    {
        const std::string w     = word();
        f64               v     = 0;
        const auto [end, error] = std::from_chars( w.data(), w.data() + w.size(), v );
        if( w.empty() || error != std::errc {} || end != w.data() + w.size() || !std::isfinite( v ) )
        {
            return std::nullopt;
        }
        return v;
    };

    const std::optional<u32> ops = whole( 1u << 31 ), constants = whole( 1u << 31 );
    if( !ops || !constants )
    {
        return std::nullopt;
    }
    for( u32 k = 0; k < *constants; ++k )
    {
        if( word() != "c" )
        {
            return std::nullopt;
        }
        Constant made;
        if( c.integers_ )
        {
            std::vector<i64> coefficients;
            for( u32 d = 0; d < field.degree(); ++d )
            {
                i64               v     = 0;
                const std::string w     = word();
                const auto [end, error] = std::from_chars( w.data(), w.data() + w.size(), v );
                if( w.empty() || error != std::errc {} || end != w.data() + w.size() )
                {
                    return std::nullopt;
                }
                coefficients.push_back( v );
            }
            made = c.make( field.polynomial( std::move( coefficients ) ) );
        }
        else
        {
            const std::optional<f64> re = real(), im = real();
            if( !re || !im )
            {
                return std::nullopt;
            }
            made = { { *re, *im }, {} };
        }
        const std::pair<f64, f64> k_of =
            c.integers_ ? std::pair<f64, f64> { static_cast<f64>( made.exact.v ), 0 } : key( made.value );
        if( !c.constant_ids_.try_emplace( k_of, k ).second )
        {
            return std::nullopt;
        }
        c.constants_.push_back( made );
    }

    const u64 columns = rows.empty() ? 0 : rows.front().size();
    for( u32 o = 0; o < *ops; ++o )
    {
        const std::string  kind = word();
        std::optional<u32> a, b = 0;
        Op_kind            k {};
        if( kind == "x" )
        {
            k = Op_kind::Input;
            a = whole( columns );
        }
        else if( kind == "a" )
        {
            k = Op_kind::Add;
            a = whole( o );
            b = whole( o );
        }
        else if( kind == "m" )
        {
            k = Op_kind::Scale;
            a = whole( o );
            b = whole( *constants );
        }
        else if( kind == "r" )
        {
            k = Op_kind::Real;
            a = whole( o );
        }
        else if( kind == "c" )
        {
            k = Op_kind::Conj;
            a = whole( o );
        }
        else if( kind == "1" )
        {
            k = Op_kind::One;
            a = 0;
        }
        if( !a || !b || c.put( { k, *a, *b } ) != o )
        {
            return std::nullopt;
        }
    }

    for( std::string w = word();; w = word() )
    {
        if( w == "end" )
        {
            return word().empty() ? std::optional<Circuit>( std::move( c ) ) : std::nullopt;
        }
        const std::optional<u32> i = whole( rows.size() ), node = whole( *ops );
        if( w != "y" || !i || !node ||
            c.outputs.try_emplace( rows[*i], *node ).first->second != *node ) // equal rows share a node
        {
            return std::nullopt;
        }
    }
}
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include <numbers>

namespace keel
{
namespace
{
const Row k_row0 { Element { 1 } };
const Row k_row1 { Element { 2 } };

// y0 = x0 + x1, y1 = x0 - x1.
Circuit dft2( bool real )
{
    Circuit   c( real );
    const u32 x0 = c.input( 0 );
    const u32 x1 = c.input( 1 );
    c.outputs.emplace( k_row0, c.add( x0, x1 ) );
    c.outputs.emplace( k_row1, c.add( x0, c.scale( x1, -1.0 ) ) );
    return c;
}

Cost scaled_cost( std::complex<f64> k )
{
    Circuit c( false );
    c.outputs.emplace( k_row0, c.scale( c.input( 0 ), k ) );
    return c.cost();
}
} // namespace

TEST_CASE( "circuit_hash_conses_nodes", "[linear][circuit]" )
{
    Circuit   c( false );
    const u32 x0 = c.input( 0 );
    const u32 x1 = c.input( 1 );
    CHECK( c.input( 0 ) == x0 );
    CHECK( c.add( x0, x1 ) == c.add( x1, x0 ) );
    CHECK( c.scale( x0, 0.5 ) == c.scale( x0, 0.5 + 1e-13 ) );
    CHECK( c.ops().size() == 5 ); // 0.5 is -1 times -0.5
}

TEST_CASE( "circuit_folds_units_and_shares_negations", "[linear][circuit]" )
{
    Circuit   c( false );
    const u32 x = c.input( 0 );
    CHECK( c.scale( x, 1.0 ) == x );
    CHECK( c.scale( c.scale( x, { 0, 1 } ), { 0, -1 } ) == x );

    // 2x is -1 times -2x.
    const u32 minus = c.scale( x, -2.0 );
    const u32 plus  = c.scale( x, 2.0 );
    CHECK( c.ops()[plus].a == minus );
    c.outputs.emplace( k_row0, plus );
    c.outputs.emplace( k_row1, minus );
    CHECK( c.cost() == Cost { 0, 2 } );
}

TEST_CASE( "circuit_costs_by_the_constant", "[linear][circuit]" )
{
    CHECK( dft2( false ).cost() == Cost { 4, 0 } );
    CHECK( dft2( true ).cost() == Cost { 2, 0 } );
    CHECK( scaled_cost( { 0, -1 } ) == Cost { 0, 0 } );
    CHECK( scaled_cost( 0.5 ) == Cost { 0, 2 } );
    CHECK( scaled_cost( { 0, 3 } ) == Cost { 0, 2 } );
    CHECK( scaled_cost( std::complex<f64> { 1, -1 } / std::sqrt( 2.0 ) ) == Cost { 2, 2 } );
    CHECK( scaled_cost( std::polar( 1.0, -2 * std::numbers::pi / 3 ) ) == Cost { 2, 4 } );
    CHECK( scaled_cost( { 1, 0.5 } ) == Cost { 2, 2 } );
    CHECK( scaled_cost( { 0.5, -1 } ) == Cost { 2, 2 } );

    Circuit real( true );
    real.outputs.emplace( k_row0, real.scale( real.input( 0 ), 3.0 ) );
    CHECK( real.cost() == Cost { 0, 1 } );
}

TEST_CASE( "circuit_costs_a_fused_multiply_as_free", "[linear][circuit]" )
{
    // 3x + 5y + x: the add takes one of its two products; z = 3x feeding two adds keeps its own.
    Circuit   c( true );
    const u32 x = c.input( 0 );
    const u32 s = c.add( c.scale( x, 3.0 ), c.scale( c.input( 1 ), 5.0 ) );
    c.outputs.emplace( k_row0, c.add( s, x ) );
    CHECK( c.cost() == Cost { 2, 2 } );
    CHECK( c.cost( true ) == Cost { 2, 1 } );

    Circuit   d( true );
    const u32 z = d.scale( d.input( 0 ), 3.0 );
    d.outputs.emplace( k_row0, d.add( z, d.input( 1 ) ) );
    d.outputs.emplace( k_row1, d.add( z, d.input( 2 ) ) );
    CHECK( d.cost( true ) == Cost { 2, 1 } );

    // Complex: a general constant's adds take two of its four; 2i's two are taken by the add after.
    Circuit   e( false );
    const u32 g = e.scale( e.input( 0 ), std::complex<f64> { 2, 3 } );
    e.outputs.emplace( k_row0, g );
    e.outputs.emplace( k_row1, e.add( e.scale( e.input( 1 ), std::complex<f64> { 0, 2 } ), e.input( 2 ) ) );
    CHECK( e.cost() == Cost { 4, 6 } );
    CHECK( e.cost( true ) == Cost { 4, 2 } );
}

TEST_CASE( "circuit_prunes_dead_nodes_and_folds_products", "[linear][circuit]" )
{
    Circuit   c( false );
    const u32 x = c.input( 0 );
    c.add( x, c.input( 1 ) );
    c.outputs.emplace( k_row0, c.scale( c.scale( x, 2.0 ), 3.0 ) );
    CHECK( c.cost() == Cost { 0, 4 } );

    const Circuit p = c.pruned();
    CHECK( p.cost() == Cost { 0, 2 } );
    CHECK( p.ops().size() == 3 );
    const Op& negate = p.ops()[p.outputs.at( k_row0 )];
    CHECK( p.constant( negate.b ) == -1.0 );
    CHECK( p.constant( p.ops()[negate.a].b ) == -6.0 );
    CHECK( p.evaluate( { { 1, 1 } } )[p.outputs.at( k_row0 )] == std::complex<f64> { 6, 6 } );
}

TEST_CASE( "circuit_pastes_one_into_another", "[linear][circuit]" )
{
    const Circuit          sub = dft2( false );
    Circuit                c( false );
    const std::vector<u32> u = c.paste( sub, { c.input( 0 ), c.input( 1 ) } );
    const std::vector<u32> y = c.paste( sub, { u[sub.outputs.at( k_row0 )], u[sub.outputs.at( k_row1 )] } );
    CHECK( c.paste( sub, { c.input( 0 ), c.input( 1 ) } ) == u );

    // DFT2 twice doubles its input.
    c.outputs.emplace( k_row0, y[sub.outputs.at( k_row0 )] );
    c.outputs.emplace( k_row1, y[sub.outputs.at( k_row1 )] );
    const std::vector<std::complex<f64>> v = c.evaluate( { { 1, 2 }, { 3, -1 } } );
    CHECK( v[c.outputs.at( k_row0 )] == std::complex<f64> { 2, 4 } );
    CHECK( v[c.outputs.at( k_row1 )] == std::complex<f64> { 6, -2 } );
    CHECK( c.cost() == Cost { 8, 0 } );
}

TEST_CASE( "circuit_counts_only_the_parts_an_output_reaches", "[linear][circuit]" )
{
    // A real input times 0.6 + 0.8i: a multiply a part, and one when only the real part is kept.
    Circuit   r( true );
    const u32 x       = r.input( 0 );
    const u32 y       = r.scale( x, { 0.6, 0.8 } );
    r.outputs[k_row0] = y;
    CHECK( r.cost() == Cost { 0, 2 } );
    r.outputs[k_row0] = r.real_part( y );
    CHECK( r.cost() == Cost { 0, 1 } );
    CHECK( r.real_part( x ) == x );

    // A complex input's real part of the same product: 0.6 re - 0.8 im.
    Circuit c( false );
    c.outputs[k_row0] = c.real_part( c.scale( c.input( 0 ), { 0.6, 0.8 } ) );
    CHECK( c.cost() == Cost { 1, 2 } );
    CHECK( c.evaluate( { { 1, 2 } } ).back() == std::complex<f64>( 0.6 - 1.6, 0 ) );

    // A real plus a complex value adds only the real parts.
    Circuit   m( true );
    const u32 a       = m.input( 0 );
    m.outputs[k_row0] = m.add( a, m.scale( m.input( 1 ), { 0.6, 0.8 } ) );
    CHECK( m.cost() == Cost { 1, 2 } );
}

TEST_CASE( "circuit_pastes_the_conjugate_map", "[linear][circuit]" )
{
    Circuit sub( false );
    sub.outputs[k_row0] = sub.add( sub.input( 0 ), sub.scale( sub.input( 1 ), { 0.6, 0.8 } ) );
    Circuit                 c( false );
    const std::vector<u32>  m   = c.paste( sub, { c.input( 0 ), c.input( 1 ) }, true );
    const std::complex<f64> got = c.evaluate( { { 1, 0 }, { 0, 1 } } )[m[sub.outputs.at( k_row0 )]];
    CHECK( std::abs( got - std::complex<f64>( 1 + 0.8, 0.6 ) ) < 1e-12 );
}

// A call is made once, costs its callee once, evaluates as the transform, and pastes conjugated as the inverse.
TEST_CASE( "circuit_calls_a_promised_transform", "[linear][circuit]" )
{
    Circuit   sub( false );
    const u32 x0 = sub.input( 0 ), x1 = sub.input( 1 );
    const u32 call = sub.call( { 7, { 10, 3 }, false, { x0, x1, Circuit::k_zero, Circuit::k_zero } } );
    REQUIRE( sub.call( { 7, { 10, 3 }, false, { x0, x1, Circuit::k_zero, Circuit::k_zero } } ) == call );
    for( u32 k = 0; k < 3; ++k )
    {
        sub.outputs[Row { Element { k } }] = sub.result( call, k );
    }
    CHECK( sub.cost() == Cost { 10, 3 } );

    const std::complex<f64>              i { 0, 1 };
    const std::vector<std::complex<f64>> v = sub.evaluate( { 1.0, i } );
    // y[k] = 1 + i W_4^k
    for( u32 k = 0; k < 3; ++k )
    {
        CHECK(
            std::abs(
                v[sub.outputs.at( Row { Element { k } } )] - ( 1.0 + i * std::polar( 1.0, -std::numbers::pi * k / 2 ) )
            ) < 1e-12
        );
    }

    Circuit                c( false );
    const std::vector<u32> m = c.paste( sub, { c.input( 0 ), c.input( 1 ) }, true );
    REQUIRE( c.calls().size() == 1 );
    CHECK( c.calls()[0].conjugate );
    const std::vector<std::complex<f64>> w = c.evaluate( { 1.0, i } );
    CHECK( std::abs( w[m[sub.outputs.at( Row { Element { 1 } } )]] - ( 1.0 + i * i ) ) < 1e-12 );

    const Circuit p = sub.pruned();
    CHECK( p.calls().size() == 1 );
    CHECK( p.cost() == Cost { 10, 3 } );
}

TEST_CASE( "circuit_keeps_integer_constants_exact", "[linear][circuit]" )
{
    Field     z( Ring_kind::Integer, 1, 8 );
    Circuit   c( z );
    const u32 x = c.input( 0 );
    CHECK( c.scale( x, z.rational( 3 ) ) == c.scale( c.scale( x, z.rational( -3 ) ), z.minus_one() ) );

    // 171 inverts 3 mod 256, so the product folds away.
    c.outputs.emplace( k_row0, c.scale( c.scale( x, z.rational( 3 ) ), z.rational( 171 ) ) );
    CHECK( c.cost() == Cost { 0, 2 } );
    const Circuit p = c.pruned();
    CHECK( p.ops()[p.outputs.at( k_row0 )].kind == Op_kind::Input );
    CHECK( p.cost() == Cost { 0, 0 } );
}

TEST_CASE( "a_bound_input_folds_into_free_constants", "[linear][circuit]" )
{
    // 3 x0 + 5 x1 + x0 with x1 = 1: one multiply and two adds, 5 a literal.
    Circuit   c( true );
    const u32 x = c.input( 0 );
    c.outputs.emplace( k_row0, c.add( c.add( c.scale( x, 3.0 ), c.scale( c.input( 1 ), 5.0 ) ), x ) );
    c.outputs.emplace( k_row1, c.add( c.scale( c.input( 1 ), 2.0 ), c.scale( c.input( 1 ), -2.0 ) ) );
    const Circuit b = c.bound( 1 );
    CHECK( b.cost() == Cost { .adds = 2, .muls = 1 } );
    const std::vector<std::complex<f64>> v = b.evaluate( { 2.0 } );
    CHECK( v[b.outputs.at( k_row0 )] == 13.0 );
    CHECK( v[b.outputs.at( k_row1 )] == 0.0 );
}
} // namespace keel
#endif // ENABLE_UNIT_TESTS
