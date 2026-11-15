// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linear/sums.h"
#include <bit>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <optional>
#include <utility>
#include "linear/circuit_cache.h"
#include "linear/field.h"

namespace keel
{
namespace
{
using i128 = __int128;
using u128 = unsigned __int128;

// y[k] = sum over j < n of x[j] W_M^(c j k), for k < m: the shape the index rules take.
struct Kernel
{
    u32 n;
    u32 m;
    u64 M;
    u64 c;
};

Sum kernel_sum( const Kernel& k, bool real )
{
    Sum s  = exponent_kernel( k.n, k.m, k.M, k.c );
    s.real = real;
    return s;
}

u64 mod( i64 v, u64 M )
{
    const i128 r = i128 { v } % i128 { M };
    return static_cast<u64>( r < 0 ? r + M : r );
}

// e at (j, k), mod M.
u64 exponent_at( const Exponent& e, u64 M, u64 j, u64 k )
{
    u64 total = 0;
    for( const auto& [powers, c] : e )
    {
        u64 term = mod( c, M );
        for( u8 p = 0; p < powers[0]; ++p )
        {
            term = static_cast<u64>( u128 { term } * ( j % M ) % M );
        }
        for( u8 q = 0; q < powers[1]; ++q )
        {
            term = static_cast<u64>( u128 { term } * ( k % M ) % M );
        }
        total = static_cast<u64>( ( u128 { total } + term ) % M );
    }
    return total;
}

std::complex<f64> root( u64 e, u64 M )
{
    return std::polar( 1.0, -2 * std::numbers::pi * static_cast<f64>( e % M ) / static_cast<f64>( M ) );
}

f64 value( Rational c )
{
    return static_cast<f64>( c.num ) / static_cast<f64>( c.den );
}

// The coefficient of x[j] in y[k].
std::complex<f64> entry( const Sum& s, u64 j, u64 k )
{
    std::complex<f64> v = 0;
    for( const auto& [e, c] : s.coefficient )
    {
        v += value( c ) * root( exponent_at( e, s.M, j, k ), s.M );
    }
    return v;
}

bool is_zero( std::complex<f64> v )
{
    return std::abs( v ) < 1e-12;
}

// A term with each power's coefficient below M, and the constant below M/2, as W^(M/2) = -1.
std::pair<Exponent, Rational> canonical( const Exponent& e, Rational c, u64 M )
{
    Exponent r;
    for( const auto& [powers, v] : e )
    {
        if( const u64 x = mod( v, M ); x != 0 )
        {
            r[powers] = static_cast<i64>( x );
        }
    }
    if( const auto it = r.find( { 0, 0 } ); M % 2 == 0 && it != r.end() && static_cast<u64>( it->second ) >= M / 2 )
    {
        it->second -= static_cast<i64>( M / 2 );
        c = -c;
        if( it->second == 0 )
        {
            r.erase( it );
        }
    }
    return { r, c };
}

// Terms canonical and merged, and M the least that writes them.
Sum normalised( const Sum& s )
{
    Sum out { s.n, s.m, s.M, {}, s.real };
    u64 g = s.M;
    for( const auto& [e, c] : s.coefficient )
    {
        for( const auto& [powers, v] : canonical( e, c, s.M ).first )
        {
            g = std::gcd( g, static_cast<u64>( v ) );
        }
    }
    out.M = s.M / g;
    for( const auto& [e, c] : s.coefficient )
    {
        Exponent scaled;
        for( const auto& [powers, v] : canonical( e, c, s.M ).first )
        {
            scaled[powers] = v / static_cast<i64>( g );
        }
        const auto [r, d] = canonical( scaled, canonical( e, c, s.M ).second, out.M );
        Rational& t       = out.coefficient[r];
        t                 = t + d;
        if( t == Rational {} )
        {
            out.coefficient.erase( r );
        }
    }
    return out;
}

// Every exponent negated: the conjugate map, so the conjugate circuit at the same cost.
Sum conjugate_sum( const Sum& s )
{
    Sum c { s.n, s.m, s.M, {}, s.real };
    for( const auto& [e, v] : s.coefficient )
    {
        Exponent negated;
        for( const auto& [powers, x] : e )
        {
            negated[powers] = -x;
        }
        const auto [r, d] = canonical( negated, v, s.M );
        c.coefficient[r]  = d;
    }
    return c;
}

// The sum or its conjugate, whichever orders first, and whether it is the conjugate.
std::pair<Sum, bool> representative( const Sum& s )
{
    Sum c = conjugate_sum( s );
    return c < s ? std::pair { std::move( c ), true } : std::pair { s, false };
}

std::optional<Kernel> kernel_of( const Sum& s )
{
    if( s.coefficient.size() != 1 || s.coefficient.begin()->second != 1 )
    {
        return std::nullopt;
    }
    const Exponent& e = s.coefficient.begin()->first;
    if( e.empty() )
    {
        return Kernel { s.n, s.m, s.M, 0 };
    }
    if( e.size() != 1 || e.begin()->first != std::array<u8, 2> { 1, 1 } )
    {
        return std::nullopt;
    }
    return Kernel { s.n, s.m, s.M, static_cast<u64>( e.begin()->second ) };
}

Cost twiddle_cost( u64 e, u64 M )
{
    return e % M == 0 ? Cost {} : scale_cost( root( e, M ), false );
}

std::string sides( const Sum& s )
{
    return std::to_string( s.n ) + "x" + std::to_string( s.m );
}

// The adds that sum m complex outputs pairwise, `count` times.
Cost output_adds( u32 m, u32 count )
{
    return { 2 * m * count, 0 };
}

// The circuit of a part, pasted onto its inputs.
using Part_builder = std::function<std::vector<u32>( Circuit&, const Sum&, const std::vector<u32>& )>;

// One way a rule takes a sum: `count` copies of each part, and what the rule adds to them.
struct Use
{
    std::vector<std::pair<u32, Sum>> parts;
    Cost                             own;
    std::string                      how;
    std::optional<Planned>           leaf {};
    std::array<u64, 2>               chosen {}; // sizes the rule picked
};

// A fact the planner may use. `match` gives each way it takes a sum, its side conditions met;
// `build` makes the sum from one of them and its parts' circuits.
struct Rule
{
    const char* name;
    const char* identity;
    std::vector<Use> ( *match )( const Sum&, const Plan_options& );
    std::vector<u32> ( *build )( Circuit&, const Sum&, const Use&, const std::vector<u32>&, const Part_builder& );
};

// Every product, summed in j order; a row of zeros is x[0] times 0.
std::vector<u32> direct( Circuit& c, const Sum& s, const std::vector<u32>& x )
{
    std::vector<u32> out;
    for( u32 o = 0; o < s.m; ++o )
    {
        std::optional<u32> acc;
        for( u32 j = 0; j < s.n; ++j )
        {
            if( const std::complex<f64> v = entry( s, j, o ); !is_zero( v ) )
            {
                const u32 term = c.scale( x[j], v );
                acc            = acc ? c.add( *acc, term ) : term;
            }
        }
        out.push_back( acc ? *acc : c.scale( x[0], 0.0 ) );
    }
    return out;
}

// Up to 2^16 products.
std::vector<Use> direct_match( const Sum& s, const Plan_options& )
{
    if( u64 { s.n } * s.m > 1 << 16 )
    {
        return {};
    }
    Cost cost {};
    for( u32 o = 0; o < s.m; ++o )
    {
        bool started = false, real = true;
        for( u32 j = 0; j < s.n; ++j )
        {
            const std::complex<f64> v = entry( s, j, o );
            if( is_zero( v ) )
            {
                continue;
            }
            const bool term = s.real && std::abs( v.imag() ) < 1e-12;
            cost            = cost + scale_cost( v, s.real );
            if( started )
            {
                cost.adds += real || term ? 1 : 2;
            }
            real    = ( !started || real ) && term;
            started = true;
        }
    }
    return { { {}, cost, "direct " + sides( s ) } };
}

std::vector<u32> direct_build( Circuit& c, const Sum& s, const Use&, const std::vector<u32>& x, const Part_builder& )
{
    return direct( c, s, x );
}

// What `circuit` costs on real inputs.
Cost on_real_inputs( const Circuit& circuit, const std::vector<u32>& outputs, u32 n )
{
    Circuit          probe( true );
    std::vector<u32> x( n );
    for( u32 j = 0; j < n; ++j )
    {
        x[j] = probe.input( j );
    }
    const std::vector<u32> m = probe.paste( circuit, x );
    for( u32 o = 0; o < outputs.size(); ++o )
    {
        probe.outputs[Row { Element { o } }] = m[outputs[o]];
    }
    return probe.cost();
}

// A side not dividing M has no structure the solver finds, and costs it minutes.
std::vector<Use> solved_match( const Sum& s, const Plan_options& options )
{
    const std::optional<Kernel> k = kernel_of( s );
    if( !k || k->n > options.leaf_max || k->m > options.leaf_max || k->M > 4 * options.leaf_max || k->n * k->m < 4 ||
        k->M % k->n != 0 || k->M % k->m != 0 )
    {
        return {};
    }
    Field            field( Ring_kind::Complex, static_cast<u32>( k->M ) );
    std::vector<Row> rows( k->m );
    for( u32 o = 0; o < k->m; ++o )
    {
        for( u32 j = 0; j < k->n; ++j )
        {
            rows[o].push_back( field.root( static_cast<u32>( k->c * j % k->M * o % k->M ) ) );
        }
    }
    const std::array<u64, 4> key { k->n, k->m, k->M, k->c };
    auto                     at = options.solved.find( key );
    if( at == options.solved.end() )
    {
        std::optional<Circuit> found =
            options.cache.empty() ? std::nullopt : cached_circuit( options.cache, field, rows, options.solve );
        if( !found )
        {
            found = solve( field, rows, options.solve ).circuit;
            if( !options.cache.empty() )
            {
                cache_circuit( options.cache, field, rows, options.solve, *found );
            }
        }
        at = options.solved.emplace( key, std::move( *found ) ).first;
    }
    Planned leaf;
    for( const Row& r : rows )
    {
        leaf.outputs.push_back( at->second.outputs.at( r ) );
    }
    leaf.cost    = s.real ? on_real_inputs( at->second, leaf.outputs, k->n ) : at->second.cost();
    leaf.circuit = at->second;
    return { { {}, leaf.cost, "solved " + sides( s ) + " W" + std::to_string( k->M ), std::move( leaf ) } };
}

std::vector<u32> solved_build( Circuit& c, const Sum&, const Use& use, const std::vector<u32>& x, const Part_builder& )
{
    const std::vector<u32> m = c.paste( use.leaf->circuit, x );
    std::vector<u32>       out;
    for( const u32 o : use.leaf->outputs )
    {
        out.push_back( m[o] );
    }
    return out;
}

std::vector<Use> periodic_match( const Sum& s, const Plan_options& )
{
    const std::optional<Kernel> k = kernel_of( s );
    if( !k || ( k->n <= k->M && k->m <= k->M ) )
    {
        return {};
    }
    const Kernel in {
        std::min<u32>( k->n, static_cast<u32>( k->M ) ), std::min<u32>( k->m, static_cast<u32>( k->M ) ), k->M, k->c
    };
    return { { { { 1, kernel_sum( in, s.real ) } }, Cost { ( s.real ? 1 : 2 ) * ( k->n - in.n ), 0 }, "periodic" } };
}

std::vector<u32>
periodic_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x0, const Part_builder& part )
{
    std::vector<u32> x  = x0;
    const Sum&       in = use.parts[0].second;
    for( u32 j = in.n; j < s.n; ++j )
    {
        x[j % in.n] = c.add( x[j % in.n], x[j] );
    }
    x.resize( in.n );
    const std::vector<u32> sub = part( c, in, x );
    std::vector<u32>       out;
    for( u32 o = 0; o < s.m; ++o )
    {
        out.push_back( sub[o % in.m] );
    }
    return out;
}

// How many j < n are r mod P.
u32 ragged( u32 n, u64 P, u64 r )
{
    return static_cast<u32>( ( n - r + P - 1 ) / P );
}

// One more of `part`.
void add_part( Use& use, Sum part )
{
    for( auto& [count, p] : use.parts )
    {
        if( p == part )
        {
            ++count;
            return;
        }
    }
    use.parts.push_back( { 1, std::move( part ) } );
}

Sum inner_sum( const Kernel& k, u64 P, u64 R, u64 j1, bool real )
{
    return kernel_sum( { ragged( k.n, P, j1 ), static_cast<u32>( R ), k.M, k.c * P % k.M }, real );
}

Sum outer_sum( const Kernel& k, u64 P, u64 R, u64 k2 )
{
    return kernel_sum( { static_cast<u32>( P ), ragged( k.m, R, k2 ), k.M, k.c * R % k.M }, false );
}

// Each P dividing n or M, with the smallest R; a P or R not dividing n or m leaves ragged parts.
std::vector<Use> split_match( const Sum& s, const Plan_options& )
{
    const std::optional<Kernel> found = kernel_of( s );
    if( !found )
    {
        return {};
    }
    const Kernel&    k = *found;
    std::vector<Use> uses;
    for( u32 P = 2; P < k.n; ++P )
    {
        if( k.n % P != 0 && k.M % P != 0 )
        {
            continue;
        }
        const u64 R = std::min<u64>( k.M / std::gcd( k.M, u64 { P } ), k.m );
        Cost      own {};
        for( u32 j1 = 1; j1 < P; ++j1 )
        {
            for( u64 k2 = 1; k2 < R; ++k2 )
            {
                own = own + twiddle_cost( k.c * j1 % k.M * k2, k.M );
            }
        }
        Use use { {}, own, "split " + std::to_string( P ) + "x" + std::to_string( k.n / P ), {}, { P, R } };
        for( u32 j1 = 0; j1 < P; ++j1 )
        {
            add_part( use, inner_sum( k, P, R, j1, s.real ) );
        }
        for( u64 k2 = 0; k2 < R; ++k2 )
        {
            add_part( use, outer_sum( k, P, R, k2 ) );
        }
        uses.push_back( std::move( use ) );
    }
    return uses;
}

std::vector<u32> split_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    const Kernel k    = *kernel_of( s );
    const auto [P, R] = use.chosen;

    std::vector<std::vector<u32>> z( P );
    for( u32 j1 = 0; j1 < P; ++j1 )
    {
        const Sum        inner = inner_sum( k, P, R, j1, s.real );
        std::vector<u32> in( inner.n );
        for( u32 j2 = 0; j2 < inner.n; ++j2 )
        {
            in[j2] = x[j1 + P * j2];
        }
        z[j1] = part( c, inner, in );
        for( u32 k2 = 0; k2 < R; ++k2 )
        {
            const u64 e = k.c * j1 % k.M * k2 % k.M;
            if( e != 0 )
            {
                z[j1][k2] = c.scale( z[j1][k2], root( e, k.M ) );
            }
        }
    }

    std::vector<u32> out( k.m );
    for( u32 k2 = 0; k2 < R; ++k2 )
    {
        std::vector<u32> in( P );
        for( u32 j1 = 0; j1 < P; ++j1 )
        {
            in[j1] = z[j1][k2];
        }
        const Sum              outer = outer_sum( k, P, R, k2 );
        const std::vector<u32> y     = part( c, outer, in );
        for( u32 k1 = 0; k1 < outer.m; ++k1 )
        {
            out[k2 + R * k1] = y[k1];
        }
    }
    return out;
}

// The least power of 2 at least n + m - 1, when M is not one, so a convolution's transforms never
// need another. Other lengths' leaves cost the solver minutes.
std::optional<u64> length( u32 n, u32 m, u64 M )
{
    if( std::has_single_bit( M ) )
    {
        return std::nullopt;
    }
    return std::bit_ceil( u64 { n } + m - 1 );
}

// The sum chirp leaves of a kernel, in M or 2M so (k - j)^2 / 2 is whole; and its half c.
std::pair<Sum, i64> chirped( const Kernel& k )
{
    const u64 M = k.c % 2 == 0 ? k.M : 2 * k.M;
    const i64 h = static_cast<i64>( k.c % 2 == 0 ? k.c / 2 : k.c );
    return { Sum { k.n, k.m, M, { { Exponent { { { 2, 0 }, -h }, { { 1, 1 }, 2 * h }, { { 0, 2 }, -h } }, 1 } } }, h };
}

// W^(c j k) = W^(c j^2 / 2) W^(-c (k - j)^2 / 2) W^(c k^2 / 2).
std::vector<Use> chirp_match( const Sum& s, const Plan_options& )
{
    const std::optional<Kernel> k = kernel_of( s );
    if( !k || k->c == 0 || k->n < 2 || k->m < 2 )
    {
        return {};
    }
    const auto [part, h] = chirped( *k );
    if( !length( k->n, k->m, part.M ) )
    {
        return {};
    }
    Cost own {};
    for( u64 j = 0; j < k->n; ++j )
    {
        own = own + scale_cost( root( mod( h, part.M ) * j % part.M * j, part.M ), s.real );
    }
    for( u64 o = 0; o < k->m; ++o )
    {
        own = own + scale_cost( root( mod( h, part.M ) * o % part.M * o, part.M ), false );
    }
    return { { { { 1, part } }, own, "chirp" } };
}

std::vector<u32> chirp_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x0, const Part_builder& part )
{
    const auto [sum, h] = chirped( *kernel_of( s ) );
    std::vector<u32> x  = x0;
    for( u64 j = 0; j < s.n; ++j )
    {
        x[j] = c.scale( x[j], root( mod( h, sum.M ) * j % sum.M * j, sum.M ) );
    }
    std::vector<u32> y = part( c, use.parts[0].second, x );
    for( u64 o = 0; o < s.m; ++o )
    {
        y[o] = c.scale( y[o], root( mod( h, sum.M ) * o % sum.M * o, sum.M ) );
    }
    return y;
}

// Whether e(j, k) is q(k - j) for q(d) = e(0, d), mod M.
bool in_difference( const Exponent& e, u64 M )
{
    Exponent expanded;
    for( const auto& [powers, v] : e )
    {
        if( powers[0] != 0 )
        {
            continue;
        }
        i64 binomial = 1;
        for( u8 i = 0; i <= powers[1]; ++i )
        {
            const i128 term = i128 { v } * binomial * ( i % 2 == 0 ? 1 : -1 );
            i64&       at   = expanded[{ i, static_cast<u8>( powers[1] - i ) }];
            at              = static_cast<i64>( mod( static_cast<i64>( ( i128 { at } + term ) % M ), M ) );
            binomial        = binomial * ( powers[1] - i ) / ( i + 1 );
        }
    }
    return canonical( expanded, 1, M ).first == canonical( e, 1, M ).first;
}

// A sum in k - j is a convolution: zero-padded to a cyclic one of length L, it is DFT_L, a product
// with the kernel's transform, and DFT_L back.
std::vector<Use> convolution_match( const Sum& s, const Plan_options& )
{
    if( s.coefficient.size() != 1 )
    {
        return {};
    }
    const Exponent& e = s.coefficient.begin()->first;
    if( !e.contains( { 1, 1 } ) || !in_difference( e, s.M ) )
    {
        return {};
    }
    const std::optional<u64> L = length( s.n, s.m, s.M );
    if( !L )
    {
        return {};
    }
    const u32 l = static_cast<u32>( *L );
    return {
        { { { 1, kernel_sum( { s.n, l, *L, 1 }, s.real ) }, { 1, kernel_sum( { l, s.m, *L, *L - 1 }, false ) } },
          l * scale_cost( { 0.3, 0.7 }, false ),
          "convolution " + std::to_string( *L ),
          {},
          { *L, 0 } }
    };
}

std::vector<u32>
convolution_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    const u64                      L = use.chosen[0];
    std::vector<std::complex<f64>> h( L ), W( L ), H( L );
    for( i64 t = 1 - i64 { s.n }; t < i64 { s.m }; ++t )
    {
        h[mod( t, L )] = t >= 0 ? entry( s, 0, static_cast<u64>( t ) ) : entry( s, static_cast<u64>( -t ), 0 );
    }
    for( u64 i = 0; i < L; ++i )
    {
        W[i] = root( i, L );
    }
    for( u64 f = 0; f < L; ++f )
    {
        for( i64 t = 1 - i64 { s.n }; t < i64 { s.m }; ++t )
        {
            const u64 at = mod( t, L );
            H[f] += h[at] * W[f * at % L];
        }
        H[f] /= static_cast<f64>( L );
    }
    std::vector<u32> X = part( c, use.parts[0].second, x );
    for( u64 f = 0; f < L; ++f )
    {
        X[f] = c.scale( X[f], H[f] );
    }
    return part( c, use.parts[1].second, X );
}

// One part a term.
std::vector<Use> terms_match( const Sum& s, const Plan_options& )
{
    if( s.coefficient.size() < 2 )
    {
        return {};
    }
    Use use { {}, output_adds( s.m, static_cast<u32>( s.coefficient.size() - 1 ) ), "terms" };
    for( const auto& [e, c] : s.coefficient )
    {
        use.parts.push_back( { 1, Sum { s.n, s.m, s.M, { { e, c } }, s.real } } );
    }
    return { std::move( use ) };
}

std::vector<u32> terms_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    std::vector<u32> out;
    for( const auto& [count, term] : use.parts )
    {
        const std::vector<u32> y = part( c, term, x );
        for( u32 o = 0; o < s.m; ++o )
        {
            out.size() == s.m ? void( out[o] = c.add( out[o], y[o] ) ) : out.push_back( y[o] );
        }
    }
    return out;
}

// The term's powers of j alone, of k alone with the constant, and of both.
std::array<Exponent, 3> by_symbol( const Exponent& e )
{
    std::array<Exponent, 3> parts;
    for( const auto& [powers, v] : e )
    {
        parts[powers[1] == 0 && powers[0] > 0 ? 0 : powers[0] == 0 ? 1 : 2][powers] = v;
    }
    return parts;
}

// One term c W^(a(j) + b(k) + d(j, k)).
std::vector<Use> factor_match( const Sum& s, const Plan_options& )
{
    if( s.coefficient.size() != 1 )
    {
        return {};
    }
    const auto& [e, c]      = *s.coefficient.begin();
    const auto [in, out, d] = by_symbol( e );
    if( in.empty() && out.empty() && c == 1 )
    {
        return {};
    }
    Cost own {};
    for( u32 j = 0; j < s.n && !in.empty(); ++j )
    {
        own = own + scale_cost( root( exponent_at( in, s.M, j, 0 ), s.M ), s.real );
    }
    for( u32 k = 0; k < s.m; ++k )
    {
        own = own + scale_cost( value( c ) * root( exponent_at( out, s.M, 0, k ), s.M ), false );
    }
    return { { { { 1, Sum { s.n, s.m, s.M, { { d, 1 } }, s.real && in.empty() } } }, own, "factor" } };
}

std::vector<u32> factor_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x0, const Part_builder& part )
{
    const auto& [e, k]      = *s.coefficient.begin();
    const auto [in, out, d] = by_symbol( e );
    std::vector<u32> x      = x0;
    for( u32 j = 0; j < s.n && !in.empty(); ++j )
    {
        x[j] = c.scale( x[j], root( exponent_at( in, s.M, j, 0 ), s.M ) );
    }
    std::vector<u32> y = part( c, use.parts[0].second, x );
    for( u32 o = 0; o < s.m; ++o )
    {
        y[o] = c.scale( y[o], value( k ) * root( exponent_at( out, s.M, 0, o ), s.M ) );
    }
    return y;
}

// Real x: c W^e + c W^-e = 2 Re(c W^e), and a term its own conjugate is real.
std::vector<Use> conjugate_match( const Sum& s, const Plan_options& )
{
    if( !s.real )
    {
        return {};
    }
    Sum  half { s.n, s.m, s.M, {}, true };
    bool paired = false;
    for( const auto& [e, c] : s.coefficient )
    {
        Exponent negated;
        for( const auto& [powers, v] : e )
        {
            negated[powers] = -v;
        }
        const auto [partner, d] = canonical( negated, c, s.M );
        if( partner == e && d == c )
        {
            half.coefficient[e] = c;
            continue;
        }
        const auto found = s.coefficient.find( partner );
        if( found == s.coefficient.end() || found->second != d )
        {
            return {};
        }
        if( e < partner )
        {
            half.coefficient[e] = c + c;
            paired              = true;
        }
    }
    if( !paired )
    {
        return {};
    }
    return { { { { 1, half } }, {}, "conjugate" } };
}

std::vector<u32> conjugate_build( Circuit& c, const Sum&, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    std::vector<u32> y = part( c, use.parts[0].second, x );
    for( u32& v : y )
    {
        v = c.real_part( v );
    }
    return y;
}

const Rule rules[] = {
    { "direct", "y[k] = sum over j of x[j] times its coefficient", direct_match, direct_build },
    { "solved", "the dense solve computes its matrix", solved_match, solved_build },
    { "terms", "a sum of terms is the sum of each term's sum", terms_match, terms_build },
    { "factor",
      "W^(a(j) + b(k) + d(j, k)) = W^a(j) W^b(k) W^d(j, k): x[j] scaled before, y[k] after",
      factor_match,
      factor_build },
    { "conjugate",
      "for real x, the sum over x of conj(K) is conj of the sum of K, so c W^e + c W^-e is 2 Re(c W^e)",
      conjugate_match,
      conjugate_build },
    { "periodic", "W^M = 1: columns j and j + M are equal, and so are rows k and k + M", periodic_match, periodic_build },
    { "split",
      "j = j1 + P j2, k = k2 + R k1: W^(c j k) = W^(c P j2 k2) W^(c j1 k2) W^(c R j1 k1) when c P R = 0 mod M",
      split_match,
      split_build },
    { "chirp", "j k = (j^2 + k^2 - (k - j)^2) / 2", chirp_match, chirp_build },
    { "convolution",
      "sum over j of x[j] h(k - j) is a cyclic convolution once padded, and DFT_L takes one to a product",
      convolution_match,
      convolution_build },
};

// Every rule's uses ranked by their own cost plus their parts'; only the one chosen is built. A sum
// no rule takes has no choice, and neither has a use with such a part. A sum and its conjugate share
// one choice and one circuit.
class Planner
{
public:
    explicit Planner( const Plan_options& options )
        : options_( options )
    {
    }

    bool plannable( const Sum& s )
    {
        return choose( s ).rule != nullptr;
    }

    Planned build( const Sum& s0 )
    {
        const auto [s, conjugated] = representative( normalised( s0 ) );
        if( conjugated )
        {
            const Planned r = build( s );
            Planned       p;
            p.how     = "conjugate of " + r.how;
            p.circuit = Circuit( s.real );
            std::vector<u32> x( s.n );
            for( u32 j = 0; j < s.n; ++j )
            {
                x[j] = p.circuit.input( j );
            }
            const std::vector<u32> m = p.circuit.paste( r.circuit, x, true );
            for( u32 o = 0; o < s.m; ++o )
            {
                p.outputs.push_back( m[r.outputs[o]] );
                p.circuit.outputs[Row { Element { o } }] = p.outputs[o];
            }
            p.cost = p.circuit.cost();
            return p;
        }
        const Choice& choice = choose( s );
        Planned       p;
        p.how     = choice.how;
        p.circuit = Circuit( s.real );
        std::vector<u32> x( s.n );
        for( u32 j = 0; j < s.n; ++j )
        {
            x[j] = p.circuit.input( j );
        }
        p.outputs = choice.rule->build(
            p.circuit,
            s,
            choice.use,
            x,
            [this]( Circuit& c, const Sum& part, const std::vector<u32>& in ) { return paste( c, part, in ); }
        );
        for( u32 o = 0; o < s.m; ++o )
        {
            p.circuit.outputs[Row { Element { o } }] = p.outputs[o];
        }
        p.cost = p.circuit.cost();
        return p;
    }

private:
    struct Choice
    {
        const Rule* rule = nullptr;
        Use         use;
        Cost        cost;
        std::string how;
    };

    const Choice& choose( const Sum& s0 )
    {
        const Sum s = representative( normalised( s0 ) ).first;
        if( const auto found = choices_.find( s ); found != choices_.end() )
        {
            return found->second;
        }
        Choice& best = choices_[s]; // none while it is chosen, so a cycle has none
        Choice  chosen;
        for( const Rule& rule : rules )
        {
            for( Use& use : rule.match( s, options_ ) )
            {
                Cost        cost = use.own;
                std::string how  = use.how;
                bool        able = true;
                for( u32 i = 0; i < use.parts.size() && able; ++i )
                {
                    const Choice& part = choose( use.parts[i].second );
                    able               = part.rule != nullptr;
                    cost               = cost + use.parts[i].first * part.cost;
                    how += ( i == 0 ? "(" : "; " ) + part.how;
                }
                if( !use.parts.empty() )
                {
                    how += ")";
                }
                if( able && ( !chosen.rule || cost < chosen.cost ) )
                {
                    chosen = { &rule, std::move( use ), cost, std::move( how ) };
                }
            }
        }
        best = std::move( chosen );
        return best;
    }

    // The part's circuit, built once, pasted onto `x`.
    std::vector<u32> paste( Circuit& c, const Sum& part, const std::vector<u32>& x )
    {
        const auto [key, conjugated] = representative( normalised( part ) );
        auto it                      = built_.find( key );
        if( it == built_.end() )
        {
            it = built_.emplace( key, build( key ) ).first;
        }
        const std::vector<u32> m = c.paste( it->second.circuit, x, conjugated );
        std::vector<u32>       out;
        for( const u32 o : it->second.outputs )
        {
            out.push_back( m[o] );
        }
        return out;
    }

    const Plan_options&    options_;
    std::map<Sum, Choice>  choices_;
    std::map<Sum, Planned> built_;
};

i128 gcd( i128 a, i128 b )
{
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while( b != 0 )
    {
        a = std::exchange( b, a % b );
    }
    return a;
}

Rational reduced( i128 n, i128 d )
{
    const i128 g = gcd( n, d );
    n /= d < 0 ? -g : g;
    d /= d < 0 ? -g : g;
    if( n > std::numeric_limits<i64>::max() || n < -std::numeric_limits<i64>::max() || d > std::numeric_limits<i64>::max() )
    {
        throw Field::Overflow {};
    }
    Rational r;
    r.num = static_cast<i64>( n );
    r.den = static_cast<i64>( d );
    return r;
}
} // namespace

Rational::Rational( i64 n, i64 d )
{
    *this = reduced( n, d );
}

Rational Rational::operator+( Rational o ) const
{
    return reduced( i128 { num } * o.den + i128 { o.num } * den, i128 { den } * o.den );
}

Rational Rational::operator*( Rational o ) const
{
    return reduced( i128 { num } * o.num, i128 { den } * o.den );
}

Sum exponent_kernel( u32 n, u32 m, u64 M, u64 c )
{
    Sum s { n, m, M, {} };
    s.coefficient[c % M == 0 ? Exponent {} : Exponent { { { 1, 1 }, static_cast<i64>( c % M ) } }] = 1;
    return s;
}

std::optional<Planned> plan_sum( const Sum& sum, const Plan_options& options )
{
    const Sum s = normalised( sum );
    Planner   planner( options );
    if( s.coefficient.empty() || !planner.plannable( s ) )
    {
        return std::nullopt;
    }
    return planner.build( s );
}
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>

namespace keel
{
namespace
{
Planned planned( const Sum& s )
{
    const std::optional<Planned> p = plan_sum( s );
    REQUIRE( p );
    return *p;
}

// Largest error of the circuit's outputs against the sum, relative to the input's size; x is real
// when the sum says.
f64 error( const Circuit& circuit, const std::vector<u32>& outputs, const Sum& s )
{
    std::mt19937                   rng( 5 );
    std::uniform_real_distribution d( -1.0, 1.0 );
    std::vector<std::complex<f64>> x( s.n );
    for( auto& v : x )
    {
        v = { d( rng ), s.real ? 0.0 : d( rng ) };
    }
    const std::vector<std::complex<f64>> got   = circuit.evaluate( x );
    f64                                  worst = 0;
    for( u32 o = 0; o < s.m; ++o )
    {
        std::complex<f64> want = 0;
        for( u32 j = 0; j < s.n; ++j )
        {
            want += x[j] * entry( s, j, o );
        }
        worst = std::max( worst, std::abs( got[outputs[o]] - want ) );
    }
    return worst / std::sqrt( static_cast<f64>( s.n ) );
}

f64 error( const Planned& p, const Sum& s )
{
    return error( p.circuit, p.outputs, s );
}

u32 below( std::mt19937& rng, u32 n )
{
    return static_cast<u32>( rng() % n );
}

// A kernel, or terms in j, k, jk, j^2 and constants with rational coefficients; a real x's terms
// are sometimes conjugate pairs.
Sum random_sum( std::mt19937& rng )
{
    const u32  n = 1 + below( rng, 16 ), m = 1 + below( rng, 16 ), M = 1 + below( rng, 16 );
    const bool real = below( rng, 2 ) == 0;
    if( below( rng, 2 ) == 0 )
    {
        return kernel_sum( { n, m, M, below( rng, M ) }, real );
    }
    if( below( rng, 4 ) == 0 )
    {
        const i64 a = 1 + below( rng, M );
        return normalised( Sum { n, m, M, { { Exponent { { { 2, 0 }, a }, { { 1, 1 }, -2 * a }, { { 0, 2 }, a } }, 1 } }, real }
        );
    }
    Sum s { n, m, M, {}, real };
    for( u32 t = below( rng, 3 ); t < 3; ++t )
    {
        Exponent e;
        for( const std::array<u8, 2> powers : { std::array<u8, 2> { 1, 1 }, { 1, 0 }, { 0, 1 }, { 0, 0 }, { 2, 0 } } )
        {
            if( powers == std::array<u8, 2> { 1, 1 } || below( rng, 3 ) == 0 )
            {
                e[powers] = below( rng, M );
            }
        }
        const Rational c( 1 + below( rng, 3 ), 1 + below( rng, 2 ) );
        s.coefficient[e] = c;
        if( real && below( rng, 2 ) == 0 )
        {
            Exponent negated;
            for( const auto& [powers, v] : e )
            {
                negated[powers] = -v;
            }
            s.coefficient[negated] = c;
        }
    }
    return normalised( s );
}

Sum dct_ii( u32 N )
{
    const i64 M = 4 * N;
    return normalised( Sum {
        N,
        N,
        static_cast<u64>( M ),
        { { Exponent { { { 1, 1 }, 2 }, { { 0, 1 }, 1 } }, Rational( 1, 2 ) },
          { Exponent { { { 1, 1 }, M - 2 }, { { 0, 1 }, M - 1 } }, Rational( 1, 2 ) } },
        true
    } );
}

// Each rule alone, its parts summed directly, on random sums it takes.
TEST_CASE( "every_planner_rule_computes_the_naive_sum", "[linear][sums]" )
{
    std::mt19937 rng( 11 );
    Plan_options options;
    options.leaf_max = 6;
    for( const Rule& rule : rules )
    {
        INFO( rule.name << ": " << rule.identity );
        u32 checked = 0;
        for( u32 t = 0; t < 4000 && checked < 20; ++t )
        {
            const Sum s = random_sum( rng );
            if( s.coefficient.empty() )
            {
                continue;
            }
            for( const Use& use : rule.match( s, options ) )
            {
                Circuit          c( s.real );
                std::vector<u32> x( s.n );
                for( u32 j = 0; j < s.n; ++j )
                {
                    x[j] = c.input( j );
                }
                const std::vector<u32> out = rule.build(
                    c,
                    s,
                    use,
                    x,
                    []( Circuit& c, const Sum& part, const std::vector<u32>& in ) { return direct( c, part, in ); }
                );
                INFO( sides( s ) << " W" << s.M << " " << s.coefficient.size() << " terms " << use.how );
                REQUIRE( out.size() == s.m );
                CHECK( error( c, out, s ) < 1e-12 );
                ++checked;
            }
        }
        CHECK( checked > 0 );
    }
}

TEST_CASE( "a_256_point_dft_splits_to_within_1_percent_of_split_radix", "[linear][sums]" )
{
    const Sum     s = exponent_kernel( 256, 256, 256, 1 );
    const Planned p = planned( s );
    INFO( p.how << " " << p.cost.adds << "/" << p.cost.muls );
    CHECK( p.cost.adds + p.cost.muls <= 6664 * 101 / 100 );
    CHECK( error( p, s ) < 1e-12 );
}

TEST_CASE( "a_prime_dft_is_a_convolution_of_a_power_of_2", "[linear][sums]" )
{
    for( const u32 N : { 67u, 127u } )
    {
        const Sum     s = exponent_kernel( N, N, N, 1 );
        const Planned p = planned( s );
        INFO( N << " " << p.how << " " << p.cost.adds << "/" << p.cost.muls );
        CHECK( p.how.starts_with( "chirp(" ) );
        const Cost naive = direct_match( s, {} )[0].own;
        CHECK( 2 * ( p.cost.adds + p.cost.muls ) < naive.adds + naive.muls );
        CHECK( error( p, s ) < 1e-12 );
    }
}

TEST_CASE( "a_sum_no_other_rule_takes_is_summed_directly", "[linear][sums]" )
{
    const Sum     s { 96, 96, 96, { { Exponent { { { 2, 1 }, 1 } }, 1 } } };
    const Planned p = planned( s );
    CHECK( p.how.starts_with( "direct" ) );
    CHECK( error( p, s ) < 1e-12 );
    CHECK( !plan_sum( Sum { 96, 96, 96, {} } ) );
}

TEST_CASE( "a_scaled_kernel_factors_out_its_scale", "[linear][sums]" )
{
    Sum s                         = exponent_kernel( 16, 16, 16, 1 );
    s.coefficient.begin()->second = 2;
    const Planned p               = planned( s );
    INFO( p.how );
    CHECK( p.how.starts_with( "factor(" ) );
    CHECK( error( p, s ) < 1e-12 );
}

TEST_CASE( "a_kernel_longer_than_its_period_folds_its_columns", "[linear][sums]" )
{
    const Sum     s = exponent_kernel( 24, 16, 8, 3 );
    const Planned p = planned( s );
    INFO( p.how );
    CHECK( error( p, s ) < 1e-12 );
}

TEST_CASE( "a_128_point_dct_ii_pairs_its_conjugate_terms", "[linear][sums]" )
{
    const Sum     s = dct_ii( 128 );
    const Planned p = planned( s );
    INFO( p.how << " " << p.cost.adds << "/" << p.cost.muls );
    CHECK( p.how.starts_with( "conjugate(factor(" ) );
    CHECK( p.cost.adds + p.cost.muls <= ( 128 * 128 + 127 * 128 ) / 5 ); // a fifth of the direct sum
    CHECK( error( p, s ) < 1e-12 );
}

// KEEL_SUM="n M c" plans a large kernel and prints it, and "dct n" a DCT-II: keel_tests "[sums_large]".
TEST_CASE( "a_large_kernel_is_planned", "[.][sums_large]" )
{
    const char* spec = std::getenv( "KEEL_SUM" );
    Sum         s    = exponent_kernel( 4096, 4096, 4096, 1 );
    if( spec )
    {
        std::istringstream in( spec );
        if( std::string( spec ).starts_with( "dct" ) )
        {
            std::string word;
            u32         n = 0;
            in >> word >> n;
            s = dct_ii( n );
        }
        else
        {
            u32 n = 0;
            u64 M = 0, c = 0;
            in >> n >> M >> c;
            s = exponent_kernel( n, n, M, c );
        }
    }
    const auto    start = std::chrono::steady_clock::now();
    const Planned p     = planned( s );
    const f64     secs  = std::chrono::duration<f64>( std::chrono::steady_clock::now() - start ).count();
    const f64     err   = error( p, s );
    std::cout << "sum " << s.n << ": " << p.cost.adds << "/" << p.cost.muls << " plan " << secs << "s err " << err << "\n"
              << p.how << "\n";
    CHECK( err < 1e-11 );
}
} // namespace
} // namespace keel
#endif // ENABLE_UNIT_TESTS
