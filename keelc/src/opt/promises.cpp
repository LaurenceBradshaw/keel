// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/promises.h"
#include <fmt/core.h>
#include <cmath>
#include <map>
#include <numbers>
#include <variant>
#include "sema/resolver.h"

namespace keel
{
namespace
{
// For a `@dft` function's instance at `n`: how the map it computes is not y[k] = sum over j of
// x[j] e^(-2 pi i j k / n), x = re + i im and y = yr + i yi, or nothing when it is.
std::optional<std::string> breaks_dft(
    const Function&              func,
    u64                          n,
    const std::vector<Function>& functions,
    const Purity&                purity,
    const Type_table&            types,
    const Literal_pool&          literals,
    const Global_constants&      globals
)
{
    const std::variant<Recognised, Unrecognised> read = recognise( func, functions, purity, types, literals, globals );
    if( const Unrecognised* none = std::get_if<Unrecognised>( &read ) )
    {
        return fmt::format( "computes no linear map: it {}", describe( none->why ) );
    }
    const Recognised& map = std::get<Recognised>( read );

    // A path as its parameter, 0 to 3, and index.
    const auto place = [&]( const Param_path& path ) -> std::optional<std::pair<u32, u64>>
    {
        if( !path.through || path.local.v < 1 || path.local.v > 4 || path.steps.size() != 1 || !path.steps[0].index ||
            path.steps[0].element >= n )
        {
            return std::nullopt;
        }
        return std::pair { path.local.v - 1, path.steps[0].element };
    };

    // The real map from re, im to yr, yi, one row per output.
    std::vector<std::vector<f64>> got( 2 * n, std::vector<f64>( 2 * n, 0.0 ) );
    const auto                    add = [&]( const Param_path& out, const Param_path& in, f64 v )
    {
        const auto o = place( out );
        const auto i = place( in );
        if( !o || !i || o->first < 2 || i->first > 1 )
        {
            return false;
        }
        got[( o->first - 2 ) * n + o->second][i->first * n + i->second] += v;
        return true;
    };
    for( std::size_t o = 0; o < map.outputs.size(); ++o )
    {
        for( std::size_t i = 0; i < map.inputs.size(); ++i )
        {
            const std::complex<f64> v      = map.field.value( map.rows[o][i] );
            const Value_path&       out    = map.outputs[o];
            const Value_path&       in     = map.inputs[i];
            const bool              paired = in.imaginary.local.is_valid();
            const bool              fits   = paired
                                                 ? add( out.real, in.real, v.real() ) && add( out.real, in.imaginary, -v.imag() ) &&
                                        add( out.imaginary, in.real, v.imag() ) && add( out.imaginary, in.imaginary, v.real() )
                                                 : add( out.real, in.real, v.real() );
            if( !fits )
            {
                return std::string( "reads or writes something else" );
            }
        }
    }

    for( u64 k = 0; k < n; ++k )
    {
        for( u64 j = 0; j < n; ++j )
        {
            const f64 t = -2.0 * std::numbers::pi * static_cast<f64>( j * k % n ) / static_cast<f64>( n );
            const f64 c = std::cos( t );
            const f64 s = std::sin( t );
            // yr = c re - s im, yi = s re + c im
            const f64 want[2][2] = { { c, -s }, { s, c } };
            for( u32 out = 0; out < 2; ++out )
            {
                for( u32 in = 0; in < 2; ++in )
                {
                    if( std::abs( got[out * n + k][in * n + j] - want[out][in] ) > 1e-9 )
                    {
                        return fmt::format( "gives a different `{}[{}]`", out == 0 ? "yr" : "yi", k );
                    }
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace

Promises::Promises(
    const Ast&                   ast,
    Types&                       types,
    const Module_graph&          modules,
    const std::vector<Function>& functions,
    Instantiate                  instantiate,
    Want                         want
)
    : ast_( ast ),
      types_( types ),
      modules_( modules ),
      functions_( functions ),
      instantiate_( std::move( instantiate ) ),
      want_( std::move( want ) )
{
    for( Node_id id { 0 }; id.v < ast.node_count(); ++id.v )
    {
        if( ast.promise( id ) == Promise::Dft )
        {
            dfts_.push_back( id );
        }
    }
}

// The N of a `@dft` function's `f64[N]` parameters.
u64 Promises::size( Node_id dft ) const
{
    return types_.table().get( types_.type_of( ast_.params( dft )[0] ) ).count;
}

std::function<std::optional<Promised>( u64 )> Promises::for_plan( const Function& from )
{
    if( dfts_.empty() )
    {
        return {};
    }
    const File_id     file  = ast_.span( from.declaration ).file;
    const Type_table& table = types_.table();
    const u64         own =
        from.promise == Promise::Dft ? table.get( table.get( from.locals[Local_id { 1 }].type ).element ).count : ~u64 { 0 };
    return [this, file, own, memo = std::map<u64, std::optional<Promised>> {}]( u64 n ) mutable
    {
        const auto [it, fresh] = memo.try_emplace( n );
        if( fresh && n != own )
        {
            it->second = promised( file, n );
        }
        return it->second;
    };
}

std::optional<Promised> Promises::promised( File_id from, u64 n )
{
    const std::optional<u32> f = instance( from, n );
    if( !f || !want_( *f ) )
    {
        return std::nullopt;
    }
    const auto cost = costs_.find( *f );
    return cost == costs_.end() ? std::nullopt : std::optional( Promised { *f, cost->second } );
}

// The fixed one at n `from` finds, else its generic one instantiated at n.
std::optional<u32> Promises::instance( File_id from, u64 n )
{
    for( const Node_id dft : dfts_ )
    {
        if( !ast_.is_generic( dft ) && size( dft ) == n && finds_promise( modules_, ast_, from, dft ) )
        {
            for( u32 f = 0; f < functions_.size(); ++f )
            {
                if( functions_[f].declaration == dft )
                {
                    return f;
                }
            }
        }
    }
    for( const Node_id dft : dfts_ )
    {
        if( ast_.is_generic( dft ) && finds_promise( modules_, ast_, from, dft ) )
        {
            return instantiate_( { .declaration = dft, .arguments = { types_.table().size( n ) } } );
        }
    }
    return std::nullopt;
}

void Promises::planned( u32 f, std::optional<Cost> cost )
{
    if( cost )
    {
        costs_[f] = *cost;
    }
}

void Promises::check(
    const Purity&           purity,
    const Literal_pool&     literals,
    const Global_constants& globals,
    const Interner&         interner,
    Diagnostics&            diags
)
{
    for( const Node_id dft : dfts_ )
    {
        if( modules_.is_loaded( ast_.span( dft ).file ) )
        {
            continue;
        }
        std::vector<std::pair<u32, u64>> instances;
        if( ast_.is_generic( dft ) )
        {
            for( const u64 n : { u64 { 2 }, u64 { 4 }, u64 { 8 } } )
            {
                const u32 f = instantiate_( { .declaration = dft, .arguments = { types_.table().size( n ) } } );
                instances.emplace_back( f, n );
            }
        }
        else
        {
            for( u32 f = 0; f < functions_.size(); ++f )
            {
                if( functions_[f].declaration == dft )
                {
                    instances.emplace_back( f, size( dft ) );
                }
            }
        }
        for( const auto& [f, n] : instances )
        {
            const std::optional<std::string> broken =
                breaks_dft( functions_[f], n, functions_, purity, types_.table(), literals, globals );
            if( broken )
            {
                diags.error(
                    ast_.promise_span( dft ),
                    fmt::format(
                        "`{}` is not the DFT it promises: at N = {} it {}", interner.text( ast_.name( dft ) ), n, *broken
                    ),
                    "y[k] is the sum over j of x[j] e^(-2 pi i j k / N), x = re + i im and y = yr + i yi"
                );
                break;
            }
        }
    }
}
} // namespace keel
