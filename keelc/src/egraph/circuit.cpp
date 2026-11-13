// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "egraph/circuit.h"
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <span>
#include <sstream>

namespace keel
{
namespace
{
constexpr std::complex<f64> k_real_units[]    = { { 1, 0 }, { -1, 0 } };
constexpr std::complex<f64> k_complex_units[] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };

std::span<const std::complex<f64>> float_units( bool real )
{
    if( real )
    {
        return k_real_units;
    }
    return k_complex_units;
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
    assert( op.a < 1u << 31 && op.b < 1u << 31 );
    const u64 packed           = u64( op.a ) << 33 | u64( op.b ) << 2 | u64( op.kind );
    const auto [found, is_new] = memo_.try_emplace( packed, static_cast<u32>( ops_.size() ) );
    if( is_new )
    {
        ops_.push_back( op );
    }
    return found->second;
}

u32 Circuit::input( u32 j )
{
    return put( { Op_kind::Input, j, 0 } );
}

u32 Circuit::add( u32 a, u32 b )
{
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

bool Circuit::is_unit( Constant c ) const
{
    if( integers_ )
    {
        return integers_->is_unit( c.exact );
    }
    return std::ranges::any_of( float_units( real_ ), [&]( std::complex<f64> u ) { return std::abs( c.value - u ) < 1e-12; } );
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
    // Units fold into the constant, which is kept as unit times the least of its orbit, so -cx reuses cx.
    if( ops_[a].kind == Op_kind::Scale && is_unit( constants_[ops_[a].b] ) )
    {
        c = times( c, constants_[ops_[a].b] );
        a = ops_[a].a;
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
    const auto        units = float_units( real_ );
    std::complex<f64> v     = units[0];
    for( const std::complex<f64> u : units )
    {
        v = key( u * c.value ) < key( v * c.value ) ? u : v;
    }
    const u32 inner = scale_node( a, make( v * c.value ) );
    return v == 1.0 ? inner : scale_node( inner, make( 1.0 / v ) );
}

std::vector<u32> Circuit::paste( const Circuit& sub, const std::vector<u32>& inputs )
{
    assert( integers_ == sub.integers_ );
    std::vector<u32> m;
    m.reserve( sub.ops_.size() );
    for( const Op& op : sub.ops_ )
    {
        switch( op.kind )
        {
        case Op_kind::Input:
            m.push_back( inputs[op.a] );
            break;
        case Op_kind::Add:
            m.push_back( add( m[op.a], m[op.b] ) );
            break;
        case Op_kind::Scale:
            m.push_back( scale_by( m[op.a], sub.constants_[op.b] ) );
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
        else if( ops_[o].kind == Op_kind::Scale )
        {
            stack.push_back( ops_[o].a );
        }
    }
    return seen;
}

Circuit Circuit::pruned() const
{
    const std::vector<bool> lv = live();
    std::vector<u32>        users( ops_.size(), 0 );
    for( u32 o = 0; o < ops_.size(); ++o )
    {
        if( lv[o] && ops_[o].kind != Op_kind::Input )
        {
            ++users[ops_[o].a];
            users[ops_[o].b] += ops_[o].kind == Op_kind::Add;
        }
    }
    for( const auto& [row, o] : outputs )
    {
        ++users[o];
    }

    Circuit          c = integers_ ? Circuit( *integers_ ) : Circuit( real_ );
    std::vector<u32> m( ops_.size(), 0 );
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
        else if( op.kind == Op_kind::Add )
        {
            m[o] = c.add( m[op.a], m[op.b] );
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

Cost Circuit::cost() const
{
    const std::vector<bool> lv = live();
    Cost                    total;
    for( u32 o = 0; o < ops_.size(); ++o )
    {
        if( !lv[o] || ops_[o].kind == Op_kind::Input )
        {
            continue;
        }
        if( ops_[o].kind == Op_kind::Add )
        {
            total.adds += real_ ? 1 : 2;
            continue;
        }

        // Complex: real or imaginary 0+2, |Re| = |Im| or a part +-1 2+2, else 2+4. Real: 0+1.
        const Constant& c = constants_[ops_[o].b];
        const f64       x = std::abs( c.value.real() );
        const f64       y = std::abs( c.value.imag() );
        if( is_unit( c ) )
        {
            continue;
        }
        if( real_ )
        {
            total.muls += 1;
        }
        else if( x < 1e-9 || y < 1e-9 )
        {
            total.muls += 2;
        }
        else if( std::abs( x - y ) < 1e-9 || std::abs( x - 1 ) < 1e-9 || std::abs( y - 1 ) < 1e-9 )
        {
            total.adds += 2;
            total.muls += 2;
        }
        else
        {
            total.adds += 2;
            total.muls += 4;
        }
    }
    return total;
}

std::vector<std::complex<f64>> Circuit::evaluate( const std::vector<std::complex<f64>>& inputs ) const
{
    std::vector<std::complex<f64>> v;
    v.reserve( ops_.size() );
    for( const Op& op : ops_ )
    {
        switch( op.kind )
        {
        case Op_kind::Input:
            v.push_back( inputs[op.a] );
            break;
        case Op_kind::Add:
            v.push_back( v[op.a] + v[op.b] );
            break;
        case Op_kind::Scale:
            v.push_back( constants_[op.b].value * v[op.a] );
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
        }
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
        if( w != "y" || !i || !node || !c.outputs.try_emplace( rows[*i], *node ).second )
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

TEST_CASE( "circuit_hash_conses_nodes", "[egraph][circuit]" )
{
    Circuit   c( false );
    const u32 x0 = c.input( 0 );
    const u32 x1 = c.input( 1 );
    CHECK( c.input( 0 ) == x0 );
    CHECK( c.add( x0, x1 ) == c.add( x1, x0 ) );
    CHECK( c.scale( x0, 0.5 ) == c.scale( x0, 0.5 + 1e-13 ) );
    CHECK( c.ops().size() == 5 ); // 0.5 is -1 times -0.5
}

TEST_CASE( "circuit_folds_units_and_shares_negations", "[egraph][circuit]" )
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

TEST_CASE( "circuit_costs_by_the_constant", "[egraph][circuit]" )
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

TEST_CASE( "circuit_prunes_dead_nodes_and_folds_products", "[egraph][circuit]" )
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

TEST_CASE( "circuit_pastes_one_into_another", "[egraph][circuit]" )
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

TEST_CASE( "circuit_keeps_integer_constants_exact", "[egraph][circuit]" )
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
} // namespace keel
#endif // ENABLE_UNIT_TESTS
