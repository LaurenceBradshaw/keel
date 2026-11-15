// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linear/sums.h"
#include <algorithm>
#include <bit>
#include <cassert>
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

// b(k).
std::complex<f64> offset_at( const Sum& s, u64 k )
{
    std::complex<f64> v = 0;
    for( const auto& [e, c] : s.offset )
    {
        v += value( c ) * root( exponent_at( e, s.M, 0, k ), s.M );
    }
    return v;
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

using Terms = std::map<Exponent, Rational>;

// Terms and offset canonical and merged, and M the least that writes them.
Sum normalised( const Sum& s )
{
    Sum out { s.n, s.m, s.M, {}, s.real };
    u64 g = s.M;
    for( const Terms* terms : { &s.coefficient, &s.offset } )
    {
        for( const auto& [e, c] : *terms )
        {
            for( const auto& [powers, v] : canonical( e, c, s.M ).first )
            {
                g = std::gcd( g, static_cast<u64>( v ) );
            }
        }
    }
    out.M             = s.M / g;
    out.batch         = s.batch;
    const auto merged = [&]( const Terms& from, Terms& to )
    {
        for( const auto& [e, c] : from )
        {
            Exponent scaled;
            for( const auto& [powers, v] : canonical( e, c, s.M ).first )
            {
                scaled[powers] = v / static_cast<i64>( g );
            }
            const auto [r, d] = canonical( scaled, canonical( e, c, s.M ).second, out.M );
            Rational& t       = to[r];
            t                 = t + d;
            if( t == Rational {} )
            {
                to.erase( r );
            }
        }
    };
    merged( s.coefficient, out.coefficient );
    merged( s.offset, out.offset );
    return out;
}

// Every exponent negated: the conjugate map, so the conjugate circuit at the same cost.
Sum conjugate_sum( const Sum& s )
{
    Sum        c { s.n, s.m, s.M, {}, s.real };
    const auto negated = [&]( const Terms& from, Terms& to )
    {
        for( const auto& [e, v] : from )
        {
            Exponent n;
            for( const auto& [powers, x] : e )
            {
                n[powers] = -x;
            }
            const auto [r, d] = canonical( n, v, s.M );
            to[r]             = d;
        }
    };
    negated( s.coefficient, c.coefficient );
    negated( s.offset, c.offset );
    c.batch = s.batch;
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

// Whether a split, chirp or convolution of s runs as a loop, which multiplies each twiddle and chirp in full.
bool looped( const Sum& s, const Plan_options& options )
{
    return !s.real && ( s.n > options.leaf_max || s.m > options.leaf_max );
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
    bool affine  = false; // takes a sum with an offset, which no other rule does
    bool batched = false; // takes a batch, which no other rule does
};

std::vector<Use> matches( const Rule& rule, const Sum& s, const Plan_options& options )
{
    return ( s.offset.empty() || rule.affine ) && ( s.batch == 1 || rule.batched ) ? rule.match( s, options )
                                                                                   : std::vector<Use> {};
}

// One copy of a batch.
Sum single( const Sum& s )
{
    Sum one   = s;
    one.batch = 1;
    return one;
}

// Every product, summed in j order, each copy of a batch alone; a row of zeros is x[0] times 0.
std::vector<u32> direct( Circuit& c, const Sum& s, const std::vector<u32>& x )
{
    std::vector<u32> out;
    for( u32 t = 0; t < s.batch; ++t )
    {
        for( u32 o = 0; o < s.m; ++o )
        {
            std::optional<u32> acc;
            for( u32 j = 0; j < s.n; ++j )
            {
                if( const std::complex<f64> v = entry( s, j, o ); !is_zero( v ) )
                {
                    const u32 term = c.scale( x[t * s.n + j], v );
                    acc            = acc ? c.add( *acc, term ) : term;
                }
            }
            if( const std::complex<f64> b = offset_at( s, o ); !is_zero( b ) )
            {
                acc = acc ? c.add( *acc, c.scale( c.one(), b ) ) : c.scale( c.one(), b );
            }
            out.push_back( acc ? *acc : c.scale( x[t * s.n], 0.0 ) );
        }
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

bool prime_power( u64 M )
{
    u64 p = 2;
    while( M > 1 && M % p != 0 )
    {
        ++p;
    }
    while( M % p == 0 )
    {
        M /= p;
    }
    return M == 1;
}

// A side not dividing M has no structure the solver finds, and costs it minutes; so has a side below
// an M that is not a power of 2. A complex M of coprime factors is theirs by pfa; a real one keeps its symmetry.
// A unit c on a full side relabels it: output k is c = 1's output c k, or input j its input c j.
std::vector<Use> solved_match( const Sum& s, const Plan_options& options )
{
    const std::optional<Kernel> k = kernel_of( s );
    if( !k || k->n > options.leaf_max || k->m > options.leaf_max || k->M > 4 * options.leaf_max || k->n * k->m < 4 ||
        k->M % k->n != 0 || k->M % k->m != 0 || ( ( k->n < k->M || k->m < k->M ) && !std::has_single_bit( k->M ) ) ||
        ( !s.real && !prime_power( k->M ) ) )
    {
        return {};
    }
    const bool       outs = k->m == k->M, ins = !outs && k->n == k->M;
    const u64        c = std::gcd( k->c, k->M ) == 1 && ( outs || ins ) ? 1 : k->c;
    Field            field( s.real ? Ring_kind::Real : Ring_kind::Complex, static_cast<u32>( k->M ) );
    std::vector<Row> rows( k->m );
    for( u32 o = 0; o < k->m; ++o )
    {
        for( u32 j = 0; j < k->n; ++j )
        {
            rows[o].push_back( field.root( static_cast<u32>( c * j % k->M * o % k->M ) ) );
        }
    }
    const std::array<u64, 5> key { k->n, k->m, k->M, c, s.real };
    auto                     at = options.solved.find( key );
    if( at == options.solved.end() )
    {
        Solve_options solve_options = options.solve;
        if( solve_options.fuel == 0 || solve_options.fuel > options.leaf_fuel )
        {
            solve_options.fuel = options.leaf_fuel;
        }
        std::optional<Circuit> found =
            options.cache.empty() ? std::nullopt : cached_circuit( options.cache, field, rows, solve_options );
        if( !found )
        {
            found = solve( field, rows, solve_options ).circuit;
            if( !options.cache.empty() )
            {
                cache_circuit( options.cache, field, rows, solve_options, *found );
            }
        }
        at = options.solved.emplace( key, std::move( *found ) ).first;
    }
    const u64 u = c == k->c ? 1 : k->c;
    Planned   leaf;
    leaf.circuit = Circuit( s.real );
    std::vector<u32> x( k->n );
    for( u32 j = 0; j < k->n; ++j )
    {
        x[ins ? u * j % k->M : j] = leaf.circuit.input( j );
    }
    const std::vector<u32> nodes = leaf.circuit.paste( at->second, x );
    for( u32 o = 0; o < k->m; ++o )
    {
        leaf.outputs.push_back( nodes[at->second.outputs.at( rows[outs ? u * o % k->M : o] )] );
        leaf.circuit.outputs[Row { Element { o } }] = leaf.outputs.back();
    }
    leaf.cost = leaf.circuit.cost();
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

// Each inner sum with the j1 it starts at, equal ones one batch; ragged ones are longer first.
std::vector<std::pair<u32, Sum>> inner_batches( const Kernel& k, u64 P, u64 R, bool real )
{
    std::vector<std::pair<u32, Sum>> out;
    for( u32 j1 = 0; j1 < P; ++j1 )
    {
        const Sum inner = inner_sum( k, P, R, j1, real );
        if( !out.empty() && single( out.back().second ) == inner )
        {
            ++out.back().second.batch;
        }
        else
        {
            out.push_back( { j1, inner } );
        }
    }
    return out;
}

Sum outer_sum( const Kernel& k, u64 P, u64 R, u64 k2 )
{
    return kernel_sum( { static_cast<u32>( P ), ragged( k.m, R, k2 ), k.M, k.c * R % k.M }, false );
}

// Each P dividing n or M, with the smallest R; a P or R not dividing n or m leaves ragged parts.
std::vector<Use> split_match( const Sum& s, const Plan_options& options )
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
                own = own + ( looped( s, options ) ? Cost { 2, 4 } : twiddle_cost( k.c * j1 % k.M * k2, k.M ) );
            }
        }
        Use use { {}, own, "split " + std::to_string( P ) + "x" + std::to_string( k.n / P ), {}, { P, R } };
        for( const auto& [first, inner] : inner_batches( k, P, R, s.real ) )
        {
            add_part( use, inner );
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
    for( const auto& [first, inner] : inner_batches( k, P, R, s.real ) )
    {
        std::vector<u32> in;
        for( u32 t = 0; t < inner.batch; ++t )
        {
            for( u32 j2 = 0; j2 < inner.n; ++j2 )
            {
                in.push_back( x[first + t + P * j2] );
            }
        }
        const std::vector<u32> y = part( c, inner, in );
        for( u32 t = 0; t < inner.batch; ++t )
        {
            z[first + t].assign( y.begin() + t * inner.m, y.begin() + ( t + 1 ) * inner.m );
        }
    }
    for( u32 j1 = 0; j1 < P; ++j1 )
    {
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

// The inverse of a mod m, for coprime a and m.
u64 inverse_mod( u64 a, u64 m )
{
    u64 x = 1;
    while( a * x % m != 1 % m )
    {
        ++x;
    }
    return x;
}

// DFT_N for N = N1 N2 coprime, by j = N2 j1 + N1 j2 and k = N2 (N2^-1 mod N1) k1 + N1 (N1^-1 mod N2) k2.
// Not on real x: its second stage would lose the symmetry hermitian keeps.
std::vector<Use> pfa_match( const Sum& s, const Plan_options& )
{
    const std::optional<Kernel> k = kernel_of( s );
    if( !k || s.real || k->n != k->M || k->m != k->M || k->c == 0 || std::gcd( k->c, k->M ) != 1 )
    {
        return {};
    }
    std::vector<Use> uses;
    for( u64 N1 = 2; N1 < k->M; ++N1 )
    {
        const u64 N2 = k->M / N1;
        if( k->M % N1 != 0 || std::gcd( N1, N2 ) != 1 )
        {
            continue;
        }
        Sum a   = kernel_sum( { static_cast<u32>( N1 ), static_cast<u32>( N1 ), N1, k->c % N1 }, false );
        Sum b   = kernel_sum( { static_cast<u32>( N2 ), static_cast<u32>( N2 ), N2, k->c % N2 }, false );
        a.batch = static_cast<u32>( N2 );
        b.batch = static_cast<u32>( N1 );
        uses.push_back(
            { { { 1, a }, { 1, b } }, {}, "pfa " + std::to_string( N1 ) + "x" + std::to_string( N2 ), {}, { N1, N2 } }
        );
    }
    return uses;
}

std::vector<u32> pfa_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    const auto [N1, N2] = use.chosen;
    const u64        N  = s.M;
    std::vector<u32> in( N );
    for( u64 j2 = 0; j2 < N2; ++j2 )
    {
        for( u64 j1 = 0; j1 < N1; ++j1 )
        {
            in[j2 * N1 + j1] = x[( N2 * j1 + N1 * j2 ) % N];
        }
    }
    const std::vector<u32> z = part( c, use.parts[0].second, in );
    for( u64 k1 = 0; k1 < N1; ++k1 )
    {
        for( u64 j2 = 0; j2 < N2; ++j2 )
        {
            in[k1 * N2 + j2] = z[j2 * N1 + k1];
        }
    }
    const std::vector<u32> w  = part( c, use.parts[1].second, in );
    const u64              e1 = N2 * inverse_mod( N2 % N1, N1 ), e2 = N1 * inverse_mod( N1 % N2, N2 );
    std::vector<u32>       y( N );
    for( u64 k1 = 0; k1 < N1; ++k1 )
    {
        for( u64 k2 = 0; k2 < N2; ++k2 )
        {
            y[( e1 * k1 + e2 * k2 ) % N] = w[k1 * N2 + k2];
        }
    }
    return y;
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
std::vector<Use> chirp_match( const Sum& s, const Plan_options& options )
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
    Cost own = looped( s, options ) ? ( k->n + k->m ) * Cost { 2, 4 } : Cost {};
    for( u64 j = 0; j < k->n && !looped( s, options ); ++j )
    {
        own = own + scale_cost( root( mod( h, part.M ) * j % part.M * j, part.M ), s.real );
    }
    for( u64 o = 0; o < k->m && !looped( s, options ); ++o )
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

std::vector<std::complex<f64>> transform( const Sum& s, u64 L );

std::vector<u32>
convolution_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    std::vector<std::complex<f64>> H = transform( s, use.chosen[0] );
    std::vector<u32>               X = part( c, use.parts[0].second, x );
    for( u64 f = 0; f < H.size(); ++f )
    {
        X[f] = c.scale( X[f], H[f] );
    }
    return part( c, use.parts[1].second, X );
}

// The kernel h(k - j)'s DFT_L, over L, so the product's inverse needs no scaling.
std::vector<std::complex<f64>> transform( const Sum& s, u64 L )
{
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
    return H;
}

// A generator of the units mod p, or 0 when p is not prime.
u64 primitive_root( u64 p )
{
    std::vector<u64> factors;
    u64              r = p - 1;
    for( u64 q = 2; q * q <= p; ++q )
    {
        if( p % q == 0 )
        {
            return 0;
        }
    }
    for( u64 q = 2; q * q <= r; ++q )
    {
        if( r % q == 0 )
        {
            factors.push_back( q );
            while( r % q == 0 )
            {
                r /= q;
            }
        }
    }
    if( r > 1 )
    {
        factors.push_back( r );
    }
    const auto power = [p]( u64 b, u64 e )
    {
        u64 v = 1;
        for( ; e != 0; e >>= 1, b = b * b % p )
        {
            v = e & 1 ? v * b % p : v;
        }
        return v;
    };
    for( u64 g = 2; g < p; ++g )
    {
        if( std::ranges::all_of( factors, [&]( u64 q ) { return power( g, ( p - 1 ) / q ) != 1; } ) )
        {
            return g;
        }
    }
    return 0;
}

// DFT_p for prime p, inputs at j = g^a and outputs at k = g^-b, is x[0] plus a cyclic convolution of length p - 1.
std::vector<Use> rader_match( const Sum& s, const Plan_options& options )
{
    const std::optional<Kernel> k = kernel_of( s );
    if( !k || k->c == 0 || k->n != k->M || k->m != k->M || k->M < 3 || k->M > 1 << 20 || primitive_root( k->M ) == 0 )
    {
        return {};
    }
    const u32  L   = static_cast<u32>( k->M - 1 );
    const Cost own = Cost { s.real ? 2u : 4u, 0 } +
                     ( looped( s, options ) ? L * Cost { 2, 4 }
                                            : scale_cost( -1.0 / L, true ) + ( L - 1 ) * scale_cost( { 0.3, 0.7 }, false ) );
    return {
        { { { 1, kernel_sum( { L, L, L, 1 }, s.real ) }, { 1, kernel_sum( { L, L, L, L - 1 }, false ) } },
          own,
          "rader " + std::to_string( k->M ) }
    };
}

std::vector<u32> rader_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    const Kernel     k = *kernel_of( s );
    const u64        p = k.M, L = p - 1, g = primitive_root( p );
    std::vector<u64> at( L ); // g^a
    at[0] = 1;
    for( u64 a = 1; a < L; ++a )
    {
        at[a] = at[a - 1] * g % p;
    }
    std::vector<u32> u( L );
    for( u64 a = 0; a < L; ++a )
    {
        u[a] = x[at[a]];
    }
    std::vector<u32>               U = part( c, use.parts[0].second, u );
    std::vector<u32>               V( L );
    std::vector<std::complex<f64>> w( L ), W( L );
    for( u64 t = 0; t < L; ++t )
    {
        w[t] = root( k.c * at[( L - t ) % L] % p, p );
        W[t] = root( t, L );
    }
    for( u64 f = 0; f < L; ++f )
    {
        std::complex<f64> H = 0;
        for( u64 t = 0; t < L; ++t )
        {
            H += w[t] * W[f * t % L];
        }
        V[f] = c.scale( U[f], H / static_cast<f64>( L ) );
    }
    V[0]               = c.add( V[0], x[0] );
    std::vector<u32> v = part( c, use.parts[1].second, V );
    std::vector<u32> y( p );
    y[0] = c.add( x[0], U[0] );
    for( u64 b = 0; b < L; ++b )
    {
        y[at[( L - b ) % L]] = v[b];
    }
    return y;
}

// DFT_M taking x[j] for j < n, zero past it, and giving y[k] for k < m, by a function that promises it.
std::vector<Use> promised_match( const Sum& s, const Plan_options& options )
{
    const std::optional<Kernel> k = kernel_of( s );
    if( !options.promised || !k || k->c != 1 || k->M <= options.leaf_max || k->n > k->M || k->m > k->M )
    {
        return {};
    }
    const std::optional<Promised> p = options.promised( k->M );
    if( !p )
    {
        return {};
    }
    Use use { {}, p->cost, "call " + sides( s ) + " W" + std::to_string( k->M ) };
    use.chosen = { p->callee, k->M };
    return { std::move( use ) };
}

std::vector<u32> promised_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& )
{
    std::vector<u32> arguments( use.chosen[1], Circuit::k_zero );
    std::copy( x.begin(), x.end(), arguments.begin() );
    const u32        call = c.call( { static_cast<u32>( use.chosen[0] ), use.own, false, std::move( arguments ) } );
    std::vector<u32> out;
    for( u32 o = 0; o < s.m; ++o )
    {
        out.push_back( c.result( call, o ) );
    }
    return out;
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

// e with symbol `at` (0 for j, 1 for k) replaced by a times it plus b, mod M.
Exponent substituted( const Exponent& e, u8 at, i64 a, i64 b, u64 M )
{
    Exponent out;
    for( const auto& [powers, v] : e )
    {
        i128 binomial = 1;
        for( u8 i = 0; i <= powers[at]; ++i )
        {
            i128 term = i128 { v } % M * binomial % M;
            for( u8 t = 0; t < i; ++t )
            {
                term = term * a % M;
            }
            for( u8 t = i; t < powers[at]; ++t )
            {
                term = term * b % M;
            }
            std::array<u8, 2> to = powers;
            to[at]               = i;
            i64& sum             = out[to];
            sum                  = static_cast<i64>( mod( static_cast<i64>( ( i128 { sum } + term ) % M ), M ) );
            binomial             = binomial * ( powers[at] - i ) / ( i + 1 );
        }
    }
    return out;
}

// The sum's terms with symbol `at` replaced by a times it plus b.
Sum reindexed( const Sum& s, u8 at, i64 a, i64 b )
{
    Sum out { s.n, s.m, s.M, {}, s.real };
    for( const auto& [e, c] : s.coefficient )
    {
        Rational& t = out.coefficient[substituted( e, at, a, b, s.M )];
        t           = t + c;
    }
    return normalised( out );
}

// -s.
Sum negated( const Sum& s )
{
    Sum r = s;
    for( auto& [e, c] : r.coefficient )
    {
        c = -c;
    }
    return normalised( r );
}

// Each t < below with a t = b mod M.
std::vector<u64> solutions( i64 a, i64 b, u64 M, u64 below )
{
    std::vector<u64> out;
    for( u64 t = 0; t < below; ++t )
    {
        if( mod( static_cast<i64>( ( i128 { a } * static_cast<i64>( t ) - b ) % M ), M ) == 0 )
        {
            out.push_back( t );
        }
    }
    return out;
}

i64 power( const Exponent& e, std::array<u8, 2> p )
{
    const auto it = e.find( p );
    return it == e.end() ? 0 : it->second;
}

// Columns f(j) = +-f(2n - 1 - j): x[j] at j' = j / 2 for even j, n - 1 - (j - 1) / 2 for odd, gives f(2 j').
// Use::chosen[0] is 1 for the minus sign, which odd j's x takes.
std::vector<Use> reflect_match( const Sum& s, const Plan_options& )
{
    const Sum even = reindexed( s, 0, 2, 0 );
    if( s.n < 2 || even == normalised( s ) )
    {
        return {};
    }
    const Sum odd = reindexed( s, 0, -2, 2 * i64 { s.n } - 1 );
    if( odd != even && odd != negated( even ) )
    {
        return {};
    }
    return { { { { 1, even } }, {}, "reflect", std::nullopt, { odd != even, 0 } } };
}

std::vector<u32> reflect_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    std::vector<u32> v( s.n );
    for( u32 j = 0; j < s.n; ++j )
    {
        v[j % 2 == 0 ? j / 2 : s.n - 1 - ( j - 1 ) / 2] = j % 2 == 1 && use.chosen[0] ? c.scale( x[j], -1.0 ) : x[j];
    }
    return part( c, use.parts[0].second, v );
}

// f(a - j) = +-f(j) with both columns below n: x[a - j] added to x[j], or subtracted, keeping the columns up to
// a / 2 when the pairs reach n - 1, or from a / 2 when they reach 0. Use::chosen holds a, and the sign and side.
std::vector<Use> fold_match( const Sum& s, const Plan_options& )
{
    const auto first = std::ranges::find_if( s.coefficient, []( const auto& t ) { return power( t.first, { 1, 1 } ) != 0; } );
    if( s.n < 2 || first == s.coefficient.end() )
    {
        return {};
    }
    // a takes jk to -jk and the k power C to A a + C, so the term goes to one with -A and A a + C.
    const i64        A = power( first->first, { 1, 1 } ), C = power( first->first, { 0, 1 } );
    std::vector<u64> centres;
    for( const auto& [e, c] : s.coefficient )
    {
        if( mod( power( e, { 1, 1 } ) + A, s.M ) == 0 )
        {
            for( const u64 a : solutions( A, power( e, { 0, 1 } ) - C, s.M, 2 * u64 { s.n } - 2 ) )
            {
                centres.push_back( a );
            }
        }
    }
    std::ranges::sort( centres );
    centres.erase( std::ranges::unique( centres ).begin(), centres.end() );
    std::vector<Use> uses;
    for( const u64 a : centres )
    {
        const u64 lo = a >= s.n ? a - ( s.n - 1 ) : 0, hi = std::min<u64>( s.n - 1, a );
        const Sum r = reindexed( s, 0, -1, static_cast<i64>( a ) );
        if( hi <= lo || ( r != normalised( s ) && r != negated( s ) ) )
        {
            continue;
        }
        const bool minus = r != normalised( s );
        const Cost own { static_cast<u32>( ( hi - lo + 1 ) / 2 ) * ( s.real ? 1u : 2u ), 0 };
        if( hi == s.n - 1 )
        {
            Sum kept = s;
            kept.n   = static_cast<u32>( a / 2 + 1 );
            uses.push_back( { { { 1, kept } }, own, "fold " + std::to_string( a ), std::nullopt, { a, minus } } );
        }
        if( lo == 0 )
        {
            Sum kept = reindexed( s, 0, 1, static_cast<i64>( ( a + 1 ) / 2 ) );
            kept.n   = static_cast<u32>( s.n - ( a + 1 ) / 2 );
            uses.push_back( { { { 1, kept } }, own, "fold " + std::to_string( a ), std::nullopt, { a, 2u | minus } } );
        }
    }
    return uses;
}

std::vector<u32> fold_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    const u64        a    = use.chosen[0];
    const u32        from = use.chosen[1] & 2 ? static_cast<u32>( ( a + 1 ) / 2 ) : 0;
    std::vector<u32> v( x.begin() + from, x.begin() + from + use.parts[0].second.n );
    for( u32 j = from; j < from + v.size(); ++j )
    {
        if( a >= j && a - j < s.n && a - j != j )
        {
            v[j - from] = c.add( x[j], c.scale( x[a - j], use.chosen[1] & 1 ? -1.0 : 1.0 ) );
        }
    }
    return part( c, use.parts[0].second, v );
}

// A power b j beside a j k, b = a t: W^(a j (k + t)), so y[k] is output k + t of the sum with k - t for k.
std::vector<Use> shift_match( const Sum& s, const Plan_options& )
{
    const auto first = std::ranges::find_if(
        s.coefficient, []( const auto& t ) { return power( t.first, { 1, 1 } ) != 0 && power( t.first, { 1, 0 } ) != 0; }
    );
    if( first == s.coefficient.end() )
    {
        return {};
    }
    const std::vector<u64> t = solutions( power( first->first, { 1, 1 } ), power( first->first, { 1, 0 } ), s.M, s.M );
    if( t.empty() )
    {
        return {};
    }
    Sum part = reindexed( s, 1, 1, -static_cast<i64>( t[0] ) );
    part.m   = static_cast<u32>( s.m + t[0] );
    for( const auto& [e, c] : part.coefficient )
    {
        if( std::ranges::any_of( e, []( const auto& p ) { return p.first[1] == 0 && p.first[0] > 0; } ) )
        {
            return {};
        }
    }
    return { { { { 1, part } }, {}, "shift " + std::to_string( t[0] ), std::nullopt, { t[0], 0 } } };
}

std::vector<u32> shift_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    const std::vector<u32> y = part( c, use.parts[0].second, x );
    return { y.begin() + static_cast<i64>( use.chosen[0] ), y.begin() + static_cast<i64>( use.chosen[0] + s.m ) };
}

// The sum with j and k swapped, so its matrix transposed.
Sum swapped( const Sum& s )
{
    Sum t { s.m, s.n, s.M, {}, s.real };
    for( const auto& [e, c] : s.coefficient )
    {
        Exponent f;
        for( const auto& [p, v] : e )
        {
            f[{ p[1], p[0] }] = v;
        }
        t.coefficient[f] = c;
    }
    return normalised( t );
}

// A circuit's transpose costs its multiplies and adds less inputs plus outputs. For complex x it is the conjugate
// transpose, so of the swapped sum's conjugate; for real x the matrix must be real.
std::vector<Use> transpose_match( const Sum& s, const Plan_options& )
{
    const Sum t = s.real ? swapped( s ) : normalised( conjugate_sum( swapped( s ) ) );
    if( ( s.real && normalised( conjugate_sum( s ) ) != normalised( s ) ) ||
        representative( t ).first == representative( normalised( s ) ).first )
    {
        return {};
    }
    const u32 dim = s.real ? 1 : 2;
    return { { { { 1, t } }, Cost { s.n > s.m ? dim * ( s.n - s.m ) : 0, 0 }, "transpose" } };
}

std::vector<u32> transpose_build( Circuit& c, const Sum&, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    const Sum&       t = use.parts[0].second;
    Circuit          sub( t.real );
    std::vector<u32> in( t.n );
    for( u32 j = 0; j < t.n; ++j )
    {
        in[j] = sub.input( j );
    }
    return sub.transposed( c, part( sub, t, in ), x, t.n );
}

// Real x and f(j, -k) = conj f(j, k): y[M - k] = conj y[k], so outputs past M / 2 are conjugates.
std::vector<Use> hermitian_match( const Sum& s, const Plan_options& )
{
    const u32 half = static_cast<u32>( s.M / 2 + 1 );
    if( !s.real || s.m <= half || s.m > s.M || reindexed( s, 1, -1, 0 ) != normalised( conjugate_sum( s ) ) )
    {
        return {};
    }
    return { { { { 1, Sum { s.n, half, s.M, s.coefficient, true } } }, {}, "hermitian" } };
}

std::vector<u32>
hermitian_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    std::vector<u32> y = part( c, use.parts[0].second, x );
    for( u32 k = static_cast<u32>( y.size() ); k < s.m; ++k )
    {
        y.push_back( c.conj( y[s.M - k] ) );
    }
    return y;
}

// First, so a call wins a tie.
Sum without_offset( const Sum& s )
{
    Sum part    = s;
    part.offset = {};
    return part;
}

std::vector<Use> offset_match( const Sum& s, const Plan_options& )
{
    if( s.offset.empty() )
    {
        return {};
    }
    Cost own {};
    for( u32 k = 0; k < s.m; ++k )
    {
        if( const std::complex<f64> b = offset_at( s, k ); !is_zero( b ) )
        {
            own.adds += std::abs( b.imag() ) < 1e-12 ? 1 : 2;
        }
    }
    return { { { { 1, without_offset( s ) } }, own, "offset" } };
}

std::vector<u32> offset_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    std::vector<u32> y = part( c, use.parts[0].second, x );
    for( u32 k = 0; k < s.m; ++k )
    {
        if( const std::complex<f64> b = offset_at( s, k ); !is_zero( b ) )
        {
            y[k] = c.add( y[k], c.scale( c.one(), b ) );
        }
    }
    return y;
}

// The column j and multiple a with b(k) - r(k) = a f(j, k) for every k, r the offset `rest` leaves; a real for a real x.
std::optional<std::pair<u32, std::complex<f64>>> column_of( const Sum& s, const Sum& rest )
{
    std::vector<std::complex<f64>> b( s.m );
    for( u32 k = 0; k < s.m; ++k )
    {
        b[k] = offset_at( s, k ) - offset_at( rest, k );
    }
    for( u32 j = 0; j < s.n; ++j )
    {
        std::optional<std::complex<f64>> a;
        bool                             multiple = true;
        for( u32 k = 0; k < s.m && multiple; ++k )
        {
            const std::complex<f64> f = entry( s, j, k );
            if( !a && !is_zero( f ) )
            {
                a = b[k] / f;
            }
            multiple = is_zero( b[k] - a.value_or( 0 ) * f );
        }
        if( multiple && a && ( !s.real || std::abs( a->imag() ) < 1e-12 ) )
        {
            return std::pair { j, *a };
        }
    }
    return {};
}

// The whole offset as a column multiple, or one of its terms, the rest left to the part.
std::vector<Use> column_match( const Sum& s, const Plan_options& )
{
    std::vector<Sum> rests;
    if( !s.offset.empty() )
    {
        rests.push_back( without_offset( s ) );
    }
    for( auto it = s.offset.begin(); s.offset.size() > 1 && it != s.offset.end(); ++it )
    {
        Sum rest = s;
        rest.offset.erase( it->first );
        rests.push_back( rest );
    }
    std::vector<Use> uses;
    for( const Sum& rest : rests )
    {
        if( const auto ja = column_of( s, rest ) )
        {
            const Cost own { std::abs( ja->second.imag() ) < 1e-12 ? 1u : 2u, 0 };
            uses.push_back( { { { 1, rest } }, own, "column" } );
        }
    }
    return uses;
}

std::vector<u32> column_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x0, const Part_builder& part )
{
    const auto [j, a]  = *column_of( s, use.parts[0].second );
    std::vector<u32> x = x0;
    x[j]               = c.add( x[j], c.scale( c.one(), a ) );
    return part( c, use.parts[0].second, x );
}

std::vector<Use> batch_match( const Sum& s, const Plan_options& )
{
    if( s.batch < 2 || !s.offset.empty() )
    {
        return {};
    }
    return { { { { s.batch, single( s ) } }, {}, "batch " + std::to_string( s.batch ) } };
}

std::vector<u32> batch_build( Circuit& c, const Sum& s, const Use&, const std::vector<u32>& x, const Part_builder& part )
{
    std::vector<u32> out;
    for( u32 t = 0; t < s.batch; ++t )
    {
        const std::vector<u32> y = part( c, single( s ), { x.begin() + t * s.n, x.begin() + ( t + 1 ) * s.n } );
        out.insert( out.end(), y.begin(), y.end() );
    }
    return out;
}

// Real copies of f(j, -k) = conj f(j, k), two at a time: Z, the sum of a + i b, gives
// B[k] = (Z[k] - conj Z[-k]) / 2i and A[k] = Z[k] - i B[k], A[-k] and B[-k] their conjugates. Z
// takes every -k too, so max(m, M) outputs, at most 2m; an odd copy is summed alone.
std::vector<Use> paired_match( const Sum& s, const Plan_options& )
{
    const Sum one     = single( s );
    const u64 outputs = s.m > 1 ? std::max<u64>( s.m, s.M ) : s.m;
    if( !s.real || s.batch < 2 || !s.offset.empty() || outputs > 2 * u64 { s.m } ||
        reindexed( one, 1, -1, 0 ) != normalised( conjugate_sum( one ) ) )
    {
        return {};
    }
    Cost untangle {};
    for( u64 k = 0; k < std::min<u64>( s.m, s.M ); ++k )
    {
        if( const u64 nk = ( s.M - k ) % s.M; k != nk && ( k < nk || nk >= s.m ) )
        {
            untangle = untangle + Cost { 4, 2 };
        }
    }
    Use use {
        { { s.batch / 2, Sum { s.n, static_cast<u32>( outputs ), s.M, s.coefficient, false } } },
        ( s.batch / 2 ) * untangle,
        "paired"
    };
    if( s.batch % 2 == 1 )
    {
        use.parts.push_back( { 1, one } );
    }
    return { std::move( use ) };
}

std::vector<u32> paired_build( Circuit& c, const Sum& s, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    const std::complex<f64> i { 0, 1 };
    std::vector<u32>        out( s.batch * s.m );
    for( u32 t = 0; t + 1 < s.batch; t += 2 )
    {
        std::vector<u32> z( s.n );
        for( u32 j = 0; j < s.n; ++j )
        {
            z[j] = c.add( x[t * s.n + j], c.scale( x[( t + 1 ) * s.n + j], i ) );
        }
        const std::vector<u32> Z = part( c, use.parts[0].second, z );
        u32*                   a = &out[t * s.m];
        u32*                   b = &out[( t + 1 ) * s.m];
        for( u32 k = 0; k < s.m; ++k )
        {
            const u32 nk = static_cast<u32>( ( s.M - k % s.M ) % s.M );
            if( k >= s.M )
            {
                a[k] = a[k % s.M];
                b[k] = b[k % s.M];
            }
            else if( k == nk )
            {
                a[k] = c.real_part( Z[k] );
                b[k] = c.real_part( c.scale( Z[k], -i ) );
            }
            else if( k < nk || nk >= s.m )
            {
                b[k] = c.scale( c.add( Z[k], c.scale( c.conj( Z[nk] ), -1.0 ) ), -0.5 * i );
                a[k] = c.add( Z[k], c.scale( b[k], -i ) );
                if( nk < s.m )
                {
                    a[nk] = c.conj( a[k] );
                    b[nk] = c.conj( b[k] );
                }
            }
        }
    }
    if( s.batch % 2 == 1 )
    {
        const u32              t = s.batch - 1;
        const std::vector<u32> y = part( c, use.parts[1].second, { x.begin() + t * s.n, x.end() } );
        std::copy( y.begin(), y.end(), out.begin() + t * s.m );
    }
    return out;
}

const Rule rules[] = {
    { "offset", "y[k] + b(k) adds b(k) to each output", offset_match, offset_build, true },
    { "column", "b(k) = a f(j, k) is the sum of x with a added to x[j]", column_match, column_build, true },
    { "promised", "a function promising DFT_M computes it", promised_match, promised_build },
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
    { "reflect",
      "f(j) = +-f(2n - 1 - j): the odd j read reversed, signed, are the even columns past n, so the sum is over f(2 j')",
      reflect_match,
      reflect_build },
    { "fold", "f(a - j) = +-f(j): x[j] f(j) + x[a - j] f(a - j) = (x[j] +- x[a - j]) f(j)", fold_match, fold_build },
    { "shift", "W^(a j k + a t j) = W^(a j (k + t)): y[k] is output k + t of a sum without j alone", shift_match, shift_build },
    { "transpose",
      "a circuit run backwards computes the transposed map, conjugated for complex x, at its multiplies",
      transpose_match,
      transpose_build },
    { "hermitian", "for real x and f(j, -k) = conj f(j, k), y[M - k] = conj y[k]", hermitian_match, hermitian_build },
    { "batch", "copies on separate inputs are each summed alone", batch_match, batch_build, false, true },
    { "paired",
      "for real a, b and f(j, -k) = conj f(j, k), Z = the sum of a + i b gives A[k] = (Z[k] + conj Z[-k]) / 2 and B[k] = "
      "(Z[k] - conj Z[-k]) / 2i",
      paired_match,
      paired_build,
      false,
      true },
    { "periodic", "W^M = 1: columns j and j + M are equal, and so are rows k and k + M", periodic_match, periodic_build },
    { "pfa",
      "for coprime N1 N2 = N, j = N2 j1 + N1 j2 and k = k1 mod N1, k2 mod N2: W_N^(jk) = W_N1^(j1 k1) W_N2^(j2 k2)",
      pfa_match,
      pfa_build },
    { "split",
      "j = j1 + P j2, k = k2 + R k1: W^(c j k) = W^(c P j2 k2) W^(c j1 k2) W^(c R j1 k1) when c P R = 0 mod M",
      split_match,
      split_build },
    { "chirp", "j k = (j^2 + k^2 - (k - j)^2) / 2", chirp_match, chirp_build },
    { "rader",
      "for prime p and a generator g, W^(c g^a g^-b) = W^(c g^(a - b)): DFT_p less x[0] is a cyclic convolution of length p - "
      "1",
      rader_match,
      rader_build },
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
            std::vector<u32> x( s.n * s.batch );
            for( u32 j = 0; j < s.n * s.batch; ++j )
            {
                x[j] = p.circuit.input( j );
            }
            const std::vector<u32> m = p.circuit.paste( r.circuit, x, true );
            for( u32 o = 0; o < s.m * s.batch; ++o )
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
        std::vector<u32> x( s.n * s.batch );
        for( u32 j = 0; j < s.n * s.batch; ++j )
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
        for( u32 o = 0; o < s.m * s.batch; ++o )
        {
            p.circuit.outputs[Row { Element { o } }] = p.outputs[o];
        }
        p.cost = p.circuit.cost();
        return p;
    }

    Loop_program program( const Sum& s )
    {
        Loop_program p;
        emit( p, p.steps, s, { 0, 0, 1, s.n }, { 1, 0, 1, s.m }, false );
        p.cost = cost( p, p.steps );
        assert( p.cost <= choose( s ).cost && "a loop program costs more than its rules ranked it" );
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

    static Stride in_loop( Stride a, u32 loop )
    {
        a.loops.push_back( { loop, a.batch } );
        a.batch = 0;
        return a;
    }

    u32 buffer( Loop_program& p, u64 size )
    {
        p.buffers.push_back( size );
        return static_cast<u32>( p.buffers.size() - 1 );
    }

    // Equal tables are one.
    u32 table( Loop_program& p, std::vector<std::complex<f64>> t )
    {
        if( const auto at = std::ranges::find( p.tables, t ); at != p.tables.end() )
        {
            return static_cast<u32>( at - p.tables.begin() );
        }
        p.tables.push_back( std::move( t ) );
        return static_cast<u32>( p.tables.size() - 1 );
    }

    // Equal index tables are one.
    u32 indices( Loop_program& p, std::vector<u64> t )
    {
        if( const auto at = std::ranges::find( p.indices, t ); at != p.indices.end() )
        {
            return static_cast<u32>( at - p.indices.begin() );
        }
        p.indices.push_back( std::move( t ) );
        return static_cast<u32>( p.indices.size() - 1 );
    }

    // W_M^e for e < M, made once per M.
    u32 roots( Loop_program& p, u64 M )
    {
        const auto [it, fresh] = roots_.try_emplace( M );
        if( fresh )
        {
            std::vector<std::complex<f64>> t( M );
            for( u64 e = 0; e < M; ++e )
            {
                t[e] = root( e, M );
            }
            it->second = table( p, std::move( t ) );
        }
        return it->second;
    }

    // The leaf computing s, or its conjugate.
    u32 leaf( Loop_program& p, const Sum& s, bool conj )
    {
        const auto [it, fresh] = leaves_.try_emplace( { s, conj } );
        if( fresh )
        {
            Planned l;
            l.circuit = Circuit( s.real );
            std::vector<u32> x( s.n * s.batch );
            for( u32 j = 0; j < x.size(); ++j )
            {
                x[j] = l.circuit.input( j );
            }
            l.outputs = paste_conjugated( l.circuit, s, x, conj );
            for( u32 o = 0; o < l.outputs.size(); ++o )
            {
                l.circuit.outputs[Row { Element { o } }] = l.outputs[o];
            }
            l.cost = l.circuit.cost();
            p.leaves.push_back( std::move( l ) );
            it->second = static_cast<u32>( p.leaves.size() - 1 );
        }
        return it->second;
    }

    // Steps computing s, or its conjugate, from `in` to `out`.
    void emit( Loop_program& p, std::vector<Loop_step>& into, const Sum& s0, const Stride& in, const Stride& out, bool conj )
    {
        const auto [s, flipped] = representative( normalised( s0 ) );
        conj                    = conj != flipped;
        if( s.batch > 1 )
        {
            Loop_step loop { .kind = Loop_kind::Loop, .count = s.batch, .loop = p.loops++ };
            emit( p, loop.body, single( s ), in_loop( in, loop.loop ), in_loop( out, loop.loop ), conj );
            into.push_back( std::move( loop ) );
            return;
        }
        const Choice& choice = choose( s );
        if( looped( s, options_ ) )
        {
            if( choice.rule->build == split_build )
            {
                return emit_split( p, into, s, choice.use, in, out, conj );
            }
            if( choice.rule->build == chirp_build )
            {
                return emit_chirp( p, into, s, choice.use, in, out, conj );
            }
            if( choice.rule->build == convolution_build )
            {
                return emit_convolution( p, into, s, choice.use, in, out, conj );
            }
            if( choice.rule->build == pfa_build )
            {
                return emit_pfa( p, into, s, choice.use, in, out, conj );
            }
            if( choice.rule->build == rader_build )
            {
                return emit_rader( p, into, s, choice.use, in, out, conj );
            }
        }
        into.push_back( Loop_step { .kind = Loop_kind::Leaf, .count = s.n, .leaf = leaf( p, s, conj ), .in = in, .out = out } );
    }

    // Inner sums into z[j1 R + k2], twiddles, outer sums from z, each run of equal parts one loop.
    void emit_split(
        Loop_program&           p,
        std::vector<Loop_step>& into,
        const Sum&              s,
        const Use&              use,
        const Stride&           in,
        const Stride&           out,
        bool                    conj
    )
    {
        const Kernel k    = *kernel_of( s );
        const auto [P, R] = use.chosen;
        const u32 z       = buffer( p, P * R );
        for( const auto& [first, inner] : inner_batches( k, P, R, s.real ) )
        {
            emit(
                p,
                into,
                inner,
                { in.buffer, in.offset + first * in.step, P * in.step, in.step, in.loops },
                { z, first * R, 1, R },
                conj
            );
        }
        const u64 c = conj ? ( k.M - k.c % k.M ) % k.M : k.c % k.M;
        into.push_back( Loop_step {
            .kind  = Loop_kind::Twiddle,
            .count = static_cast<u32>( P ),
            .table = roots( p, k.M ),
            .R     = R,
            .M     = k.M,
            .c     = c,
            .out   = { z }
        } );
        for( u64 k2 = 0; k2 < R; )
        {
            Sum outer = outer_sum( k, P, R, k2 );
            u64 next  = k2 + 1;
            while( next < R && outer_sum( k, P, R, next ) == outer )
            {
                ++next;
            }
            outer.batch = static_cast<u32>( next - k2 );
            emit(
                p,
                into,
                outer,
                { z, k2, R, 1 },
                { out.buffer, out.offset + k2 * out.step, R * out.step, out.step, out.loops },
                conj
            );
            k2 = next;
        }
    }

    // x scaled by the chirp, the sum in k - j, y scaled by the chirp.
    void emit_chirp(
        Loop_program&           p,
        std::vector<Loop_step>& into,
        const Sum&              s,
        const Use&              use,
        const Stride&           in,
        const Stride&           out,
        bool                    conj
    )
    {
        const auto [sum, h] = chirped( *kernel_of( s ) );
        const auto chirp    = [&]( u32 n )
        {
            std::vector<std::complex<f64>> t( n );
            for( u64 j = 0; j < n; ++j )
            {
                t[j] = root( mod( h, sum.M ) * j % sum.M * j, sum.M );
                t[j] = conj ? std::conj( t[j] ) : t[j];
            }
            return t;
        };
        const u32 a = buffer( p, s.n ), b = buffer( p, s.m );
        into.push_back(
            Loop_step { .kind = Loop_kind::Scale, .count = s.n, .table = table( p, chirp( s.n ) ), .in = in, .out = { a } }
        );
        emit( p, into, use.parts[0].second, { a }, { b }, conj );
        into.push_back(
            Loop_step { .kind = Loop_kind::Scale, .count = s.m, .table = table( p, chirp( s.m ) ), .in = { b }, .out = out }
        );
    }

    // DFT_L of x, times the kernel's transform, DFT_L back.
    void emit_convolution(
        Loop_program&           p,
        std::vector<Loop_step>& into,
        const Sum&              s,
        const Use&              use,
        const Stride&           in,
        const Stride&           out,
        bool                    conj
    )
    {
        const u64                      L = use.chosen[0];
        std::vector<std::complex<f64>> H = transform( s, L );
        for( std::complex<f64>& v : H )
        {
            v = conj ? std::conj( v ) : v;
        }
        const u32 X = buffer( p, L );
        emit( p, into, use.parts[0].second, in, { X }, conj );
        into.push_back( Loop_step {
            .kind  = Loop_kind::Scale,
            .count = static_cast<u32>( L ),
            .table = table( p, std::move( H ) ),
            .in    = { X },
            .out   = { X }
        } );
        emit( p, into, use.parts[1].second, { X }, out, conj );
    }

    // x gathered to j2 N1 + j1, the N1-point sums, the N2-point sums across them, gathered to y.
    void emit_pfa(
        Loop_program&           p,
        std::vector<Loop_step>& into,
        const Sum&              s,
        const Use&              use,
        const Stride&           in,
        const Stride&           out,
        bool                    conj
    )
    {
        const auto [N1, N2] = use.chosen;
        const u64        N  = s.M;
        const u64        e1 = N2 * inverse_mod( N2 % N1, N1 ), e2 = N1 * inverse_mod( N1 % N2, N2 );
        std::vector<u64> from( N ), to( N );
        for( u64 a = 0; a < N1; ++a )
        {
            for( u64 b = 0; b < N2; ++b )
            {
                from[b * N1 + a]            = ( N2 * a + N1 * b ) % N;
                to[( e1 * a + e2 * b ) % N] = a * N2 + b;
            }
        }
        const u32 t = buffer( p, N ), z = buffer( p, N ), w = buffer( p, N );
        const u32 n = static_cast<u32>( N );
        into.push_back( Loop_step { .kind = Loop_kind::Gather, .count = n, .table = indices( p, from ), .in = in, .out = { t } }
        );
        emit( p, into, use.parts[0].second, { t, 0, 1, N1 }, { z, 0, 1, N1 }, conj );
        emit( p, into, use.parts[1].second, { z, 0, N1, 1 }, { w, 0, 1, N2 }, conj );
        into.push_back( Loop_step { .kind = Loop_kind::Gather, .count = n, .table = indices( p, to ), .in = { w }, .out = out }
        );
    }

    // x at g^a, its DFT_L U, U times the kernel's transform plus x[0] at 0, DFT_L back, y at g^-b; y[0] = x[0] + U[0].
    void emit_rader(
        Loop_program&           p,
        std::vector<Loop_step>& into,
        const Sum&              s,
        const Use&              use,
        const Stride&           in,
        const Stride&           out,
        bool                    conj
    )
    {
        const Kernel     k = *kernel_of( s );
        const u64        P = k.M, L = P - 1, g = primitive_root( P );
        std::vector<u64> at( L ), back( L );
        at[0] = 1;
        for( u64 a = 1; a < L; ++a )
        {
            at[a] = at[a - 1] * g % P;
        }
        for( u64 b = 0; b < L; ++b )
        {
            back[at[( L - b ) % L] - 1] = b;
        }
        std::vector<std::complex<f64>> w( L ), H( L );
        for( u64 t = 0; t < L; ++t )
        {
            w[t] = root( k.c * at[( L - t ) % L] % P, P );
        }
        for( u64 f = 0; f < L; ++f )
        {
            for( u64 t = 0; t < L; ++t )
            {
                H[f] += w[t] * root( f * t % L, L );
            }
            H[f] /= static_cast<f64>( L );
            H[f] = conj ? std::conj( H[f] ) : H[f];
        }
        const u32 n = static_cast<u32>( L );
        const u32 u = buffer( p, L ), U = buffer( p, L ), V = buffer( p, L ), v = buffer( p, L );
        into.push_back( Loop_step { .kind = Loop_kind::Gather, .count = n, .table = indices( p, at ), .in = in, .out = { u } }
        );
        emit( p, into, use.parts[0].second, { u }, { U }, conj );
        into.push_back( Loop_step { .kind = Loop_kind::Scale, .count = n, .table = table( p, H ), .in = { U }, .out = { V } } );
        into.push_back( Loop_step { .kind = Loop_kind::Add, .count = 1, .in = in, .out = { V }, .also = { V } } );
        emit( p, into, use.parts[1].second, { V }, { v }, conj );
        into.push_back( Loop_step { .kind = Loop_kind::Add, .count = 1, .in = in, .out = out, .also = { U } } );
        Stride rest = out;
        rest.offset += out.step;
        into.push_back(
            Loop_step { .kind = Loop_kind::Gather, .count = n, .table = indices( p, back ), .in = { v }, .out = rest }
        );
    }

    Cost cost( const Loop_program& p, const std::vector<Loop_step>& steps ) const
    {
        Cost total {};
        for( const Loop_step& s : steps )
        {
            switch( s.kind )
            {
            case Loop_kind::Leaf:
                total = total + p.leaves[s.leaf].cost;
                break;
            case Loop_kind::Loop:
                total = total + s.count * cost( p, s.body );
                break;
            case Loop_kind::Twiddle:
                total = total + static_cast<u32>( ( s.count - 1 ) * ( s.R - 1 ) ) * Cost { 2, 4 };
                break;
            case Loop_kind::Scale:
                total = total + s.count * Cost { 2, 4 };
                break;
            case Loop_kind::Gather:
                break;
            case Loop_kind::Add:
                total = total + s.count * Cost { 2, 0 };
                break;
            }
        }
        return total;
    }

    // The part's circuit pasted onto `x`, conjugated when `conj`.
    std::vector<u32> paste_conjugated( Circuit& c, const Sum& part, const std::vector<u32>& x, bool conj )
    {
        const auto [key, conjugated] = representative( normalised( part ) );
        auto it                      = built_.find( key );
        if( it == built_.end() )
        {
            it = built_.emplace( key, build( key ) ).first;
        }
        const std::vector<u32> m = c.paste( it->second.circuit, x, conjugated != conj );
        std::vector<u32>       out;
        for( const u32 o : it->second.outputs )
        {
            out.push_back( m[o] );
        }
        return out;
    }

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
            for( Use& use : matches( rule, s, options_ ) )
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
        return paste_conjugated( c, part, x, false );
    }

    const Plan_options&                 options_;
    std::map<Sum, Choice>               choices_;
    std::map<Sum, Planned>              built_;
    std::map<u64, u32>                  roots_;  // a program's root table of each M
    std::map<std::pair<Sum, bool>, u32> leaves_; // a program's leaf of each sum, conjugated or not
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

std::optional<Loop_program> plan_loops( const Sum& sum, const Plan_options& options )
{
    const Sum s = normalised( sum );
    Planner   planner( options );
    if( s.real || s.coefficient.empty() || !s.offset.empty() || !planner.plannable( s ) )
    {
        return std::nullopt;
    }
    return planner.program( s );
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
    std::vector<std::complex<f64>> x( s.n * s.batch );
    for( auto& v : x )
    {
        v = { d( rng ), s.real ? 0.0 : d( rng ) };
    }
    const std::vector<std::complex<f64>> got   = circuit.evaluate( x );
    f64                                  worst = 0;
    for( u32 t = 0; t < s.batch; ++t )
    {
        for( u32 o = 0; o < s.m; ++o )
        {
            std::complex<f64> want = 0;
            for( u32 j = 0; j < s.n; ++j )
            {
                want += x[t * s.n + j] * entry( s, j, o );
            }
            want += offset_at( s, o );
            worst = std::max( worst, std::abs( got[outputs[t * s.m + o]] - want ) );
        }
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
Sum linear_sum( std::mt19937& rng )
{
    const u32  n = 1 + below( rng, 16 ), m = 1 + below( rng, 16 ), M = 1 + below( rng, 16 );
    const bool real = below( rng, 2 ) == 0;
    if( below( rng, 2 ) == 0 )
    {
        return kernel_sum( { n, m, M, below( rng, M ) }, real );
    }
    // cos or sin of (2j + p)(2k + q) pi / 4n on real x, as the DCTs, DSTs and the MDCT are.
    if( below( rng, 4 ) == 0 )
    {
        const i64 p = below( rng, 2 ) == 0 ? 1 : below( rng, 2 * n ), q = below( rng, 2 * n ), M8 = 8 * n;
        Exponent  e { { { 1, 1 }, 4 }, { { 1, 0 }, 2 * q }, { { 0, 1 }, 2 * p }, { { 0, 0 }, p * q } }, neg;
        for( const auto& [powers, v] : e )
        {
            neg[powers] = -v;
        }
        const bool sine = below( rng, 2 ) == 0;
        neg[{ 0, 0 }] += sine ? 2 * n : 0;
        e[{ 0, 0 }] += sine ? 2 * n : 0;
        return normalised(
            Sum { n, m, static_cast<u64>( M8 ), { { e, Rational( 1, 2 ) }, { neg, Rational( sine ? -1 : 1, 2 ) } }, true }
        );
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

// As linear_sum, a quarter of the time a batch of 2 or 3, and a third of the time with an offset: a
// term in k, or a rational multiple of a column.
Sum random_sum( std::mt19937& rng )
{
    Sum s = linear_sum( rng );
    if( below( rng, 4 ) == 0 )
    {
        s.batch = 2 + below( rng, 2 );
    }
    if( below( rng, 3 ) != 0 )
    {
        return s;
    }
    const Rational a( 1 + below( rng, 3 ), 1 + below( rng, 2 ) );
    if( below( rng, 2 ) == 0 )
    {
        s.offset[Exponent {
            { { 0, 1 }, below( rng, static_cast<u32>( s.M ) ) }, { { 0, 0 }, below( rng, static_cast<u32>( s.M ) ) }
        }] = a;
    }
    else
    {
        const i64 j = below( rng, s.n );
        for( const auto& [e, c] : s.coefficient )
        {
            Rational& t = s.offset[substituted( e, 0, 0, j, s.M )];
            t           = t + c * a;
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
    // A call evaluates as the transform it promises.
    options.promised = []( u64 ) { return std::optional<Promised>( Promised { 0, { 1, 1 } } ); };
    for( const Rule& rule : rules )
    {
        INFO( rule.name << ": " << rule.identity );
        u32 checked = 0;
        for( u32 t = 0; t < 10000 && checked < 20; ++t )
        {
            const Sum s = random_sum( rng );
            if( s.coefficient.empty() )
            {
                continue;
            }
            for( const Use& use : matches( rule, s, options ) )
            {
                Circuit          c( s.real );
                std::vector<u32> x( s.n * s.batch );
                for( u32 j = 0; j < x.size(); ++j )
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
                REQUIRE( out.size() == s.m * s.batch );
                CHECK( error( c, out, s ) < 1e-12 );
                ++checked;
            }
        }
        CHECK( checked > 0 );
    }
}

u64 element( const Stride& a, const std::vector<u64>& index, u64 i )
{
    u64 at = a.offset + a.step * i;
    for( const auto& [loop, coefficient] : a.loops )
    {
        at += coefficient * index[loop];
    }
    return at;
}

// Runs `steps` on the buffers as the lowered loops do.
void run(
    const Loop_program&                          p,
    const std::vector<Loop_step>&                steps,
    std::vector<std::vector<std::complex<f64>>>& b,
    std::vector<u64>&                            index
)
{
    for( const Loop_step& s : steps )
    {
        switch( s.kind )
        {
        case Loop_kind::Leaf:
        {
            const Planned&                 leaf = p.leaves[s.leaf];
            std::vector<std::complex<f64>> x( s.count );
            for( u32 j = 0; j < s.count; ++j )
            {
                x[j] = b[s.in.buffer].at( element( s.in, index, j ) );
            }
            const std::vector<std::complex<f64>> v = leaf.circuit.evaluate( x );
            for( u32 o = 0; o < leaf.outputs.size(); ++o )
            {
                b[s.out.buffer].at( element( s.out, index, o ) ) = v[leaf.outputs[o]];
            }
            break;
        }
        case Loop_kind::Loop:
            for( index[s.loop] = 0; index[s.loop] < s.count; ++index[s.loop] )
            {
                run( p, s.body, b, index );
            }
            break;
        case Loop_kind::Twiddle:
            for( u64 a = 1; a < s.count; ++a )
            {
                for( u64 r = 1; r < s.R; ++r )
                {
                    b[s.out.buffer].at( a * s.R + r ) *= p.tables[s.table].at( s.c * a % s.M * r % s.M );
                }
            }
            break;
        case Loop_kind::Scale:
            for( u32 i = 0; i < s.count; ++i )
            {
                b[s.out.buffer].at( element( s.out, index, i ) ) =
                    b[s.in.buffer].at( element( s.in, index, i ) ) * p.tables[s.table].at( i );
            }
            break;
        case Loop_kind::Gather:
            for( u32 i = 0; i < s.count; ++i )
            {
                b[s.out.buffer].at( element( s.out, index, i ) ) =
                    b[s.in.buffer].at( element( s.in, index, p.indices[s.table].at( i ) ) );
            }
            break;
        case Loop_kind::Add:
            for( u32 i = 0; i < s.count; ++i )
            {
                b[s.out.buffer].at( element( s.out, index, i ) ) =
                    b[s.in.buffer].at( element( s.in, index, i ) ) + b[s.also.buffer].at( element( s.also, index, i ) );
            }
            break;
        }
    }
}

f64 error( const Loop_program& p, const Sum& s )
{
    std::mt19937                                rng( 5 );
    std::uniform_real_distribution              d( -1.0, 1.0 );
    std::vector<std::vector<std::complex<f64>>> b;
    for( const u64 size : p.buffers )
    {
        b.emplace_back( size );
    }
    b[0].resize( s.n );
    b[1].resize( s.m );
    for( auto& v : b[0] )
    {
        v = { d( rng ), d( rng ) };
    }
    std::vector<u64> index( p.loops );
    run( p, p.steps, b, index );
    f64 worst = 0;
    for( u32 o = 0; o < s.m; ++o )
    {
        std::complex<f64> want = 0;
        for( u32 j = 0; j < s.n; ++j )
        {
            want += b[0][j] * entry( s, j, o );
        }
        worst = std::max( worst, std::abs( b[1][o] - want ) );
    }
    return worst / std::sqrt( static_cast<f64>( s.n ) );
}

TEST_CASE( "a_planned_dft_runs_as_loops_over_its_leaves", "[linear][sums]" )
{
    // Forward and inverse, a power of 2 and a prime by Bluestein, its splits ragged, at 8-point leaves.
    Plan_options small;
    small.leaf_max = 8;
    for( const auto [n, c] : { std::pair { 256u, 1u }, { 256u, 255u }, { 1019u, 1u }, { 1019u, 1018u } } )
    {
        const Sum                         s = exponent_kernel( n, n, n, c );
        const std::optional<Loop_program> p = plan_loops( s, small );
        REQUIRE( p );
        INFO( n << " " << c );
        CHECK( p->loops > 0 );
        CHECK( p->leaves.size() < 12 );
        CHECK( error( *p, s ) < 1e-12 );
    }
}

bool gathers( const std::vector<Loop_step>& steps )
{
    return std::ranges::any_of( steps, []( const Loop_step& s ) { return s.kind == Loop_kind::Gather || gathers( s.body ); } );
}

TEST_CASE( "a_pfa_or_rader_plan_runs_as_loops_through_index_tables", "[linear][sums]" )
{
    // Forward and inverse at 8-point leaves: 97 is prime, 96 = 32 3; 240 = 16 15.
    Plan_options small;
    small.leaf_max = 8;
    for( const u32 n : { 97u, 240u } )
    {
        for( const u32 c : { 1u, n - 1 } )
        {
            const Sum                         s = exponent_kernel( n, n, n, c );
            const std::optional<Loop_program> p = plan_loops( s, small );
            REQUIRE( p );
            INFO( n << " " << c );
            CHECK( plan_sum( s, small )->how.find( n == 97 ? "rader " : "pfa " ) < 16 );
            CHECK( gathers( p->steps ) );
            CHECK( error( *p, s ) < 1e-12 );
        }
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

// Real split radix takes 3N/2 log N - 5N/2 + 4 adds and N/2 log N - 3N/2 + 2 multiplies, 3078 at 256.
TEST_CASE( "a_256_point_real_dft_pairs_its_halves_to_within_30_percent_of_real_split_radix", "[linear][sums]" )
{
    Sum s           = exponent_kernel( 256, 256, 256, 1 );
    s.real          = true;
    const Planned p = planned( s );
    INFO( p.how << " " << p.cost.adds << "/" << p.cost.muls );
    CHECK( p.how.find( "paired(" ) != std::string::npos );
    CHECK( p.cost.adds + p.cost.muls <= 3078 * 130 / 100 );
    CHECK( error( p, s ) < 1e-12 );
}

TEST_CASE( "a_dft_plus_a_constant_adds_it_to_x0", "[linear][sums]" )
{
    Sum        s    = exponent_kernel( 256, 256, 256, 1 );
    const Cost bare = planned( s ).cost;
    s.offset        = { { Exponent {}, Rational( 3, 2 ) }, { Exponent { { { 0, 0 }, 64 } }, Rational( 1, 2 ) } };
    const Planned p = planned( s );
    INFO( p.how );
    CHECK( p.how.starts_with( "column(" ) );
    CHECK( p.cost == bare + Cost { 2, 0 } );
    CHECK( error( p, s ) < 1e-12 );
}

TEST_CASE( "a_dft_plus_multiples_of_two_columns_adds_each_to_its_input", "[linear][sums]" )
{
    Sum        s    = exponent_kernel( 64, 64, 64, 1 );
    const Cost bare = planned( s ).cost;
    s.offset = { { Exponent { { { 0, 1 }, 5 }, { { 0, 0 }, 1 } }, Rational( 3, 2 ) }, { Exponent {}, Rational( 1, 2 ) } };
    const Planned p = planned( s );
    INFO( p.how );
    CHECK( p.how.starts_with( "column(" ) );
    CHECK( p.cost == bare + Cost { 3, 0 } );
    CHECK( error( p, s ) < 1e-12 );
}

TEST_CASE( "an_offset_no_column_matches_is_added_to_each_output", "[linear][sums]" )
{
    Sum        s    = exponent_kernel( 64, 64, 64, 1 );
    const Cost bare = planned( s ).cost;
    s.offset        = { { Exponent { { { 0, 2 }, 1 } }, 1 } };
    const Planned p = planned( s );
    INFO( p.how );
    CHECK( p.how.starts_with( "offset(" ) );
    CHECK( p.cost.adds + p.cost.muls <= bare.adds + bare.muls + 2 * 64 );
    CHECK( error( p, s ) < 1e-12 );
}

TEST_CASE( "a_prime_dft_is_a_cyclic_convolution_of_length_p_minus_1", "[linear][sums]" )
{
    for( const u32 N : { 67u, 127u } )
    {
        const Sum     s = exponent_kernel( N, N, N, 1 );
        const Planned p = planned( s );
        INFO( N << " " << p.how << " " << p.cost.adds << "/" << p.cost.muls );
        CHECK( p.how.starts_with( "rader " ) );
        const Cost naive = direct_match( s, {} )[0].own;
        CHECK( 2 * ( p.cost.adds + p.cost.muls ) < naive.adds + naive.muls );
        CHECK( error( p, s ) < 1e-12 );
    }
}

// 5 DFT_3 then 3 DFT_5, with no twiddles between.
TEST_CASE( "a_dft_of_coprime_factors_costs_its_factors_alone", "[linear][sums]" )
{
    const Sum     s = exponent_kernel( 15, 15, 15, 1 );
    const Planned p = planned( s );
    INFO( p.how << " " << p.cost.adds << "/" << p.cost.muls );
    CHECK( p.how.starts_with( "pfa " ) );
    CHECK( p.cost == 5 * planned( exponent_kernel( 3, 3, 3, 1 ) ).cost + 3 * planned( exponent_kernel( 5, 5, 5, 1 ) ).cost );
    CHECK( error( p, s ) < 1e-12 );
}

TEST_CASE( "a_unit_multiple_kernel_on_a_full_side_reuses_the_plain_leaf", "[linear][sums]" )
{
    Plan_options options;
    for( const auto& [n, m] : { std::pair<u32, u32> { 16, 16 }, { 2, 16 }, { 16, 4 } } )
    {
        INFO( n << "x" << m );
        const std::optional<Planned> plain = plan_sum( exponent_kernel( n, m, 16, 1 ), options );
        const Sum                    s     = exponent_kernel( n, m, 16, 3 );
        const std::optional<Planned> p     = plan_sum( s, options );
        REQUIRE( plain );
        REQUIRE( p );
        CHECK( p->cost == plain->cost );
        CHECK( error( *p, s ) < 1e-12 );
    }
    for( const auto& [key, circuit] : options.solved )
    {
        CHECK( ( key[3] == 1 || std::gcd( key[3], key[2] ) != 1 || ( key[0] < key[2] && key[1] < key[2] ) ) );
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
    CHECK( p.how.starts_with( "reflect(conjugate(factor(" ) );
    CHECK( p.cost.adds + p.cost.muls <= ( 128 * 128 + 127 * 128 ) / 10 ); // a tenth of the direct sum
    CHECK( error( p, s ) < 1e-12 );
}

// The rule alone, its part summed directly.
TEST_CASE( "an_odd_length_dct_ii_reflects", "[linear][sums]" )
{
    const Sum              s    = dct_ii( 15 );
    const std::vector<Use> uses = reflect_match( s, {} );
    REQUIRE( uses.size() == 1 );
    Circuit          c( true );
    std::vector<u32> x( s.n );
    for( u32 j = 0; j < s.n; ++j )
    {
        x[j] = c.input( j );
    }
    const std::vector<u32> out = reflect_build(
        c, s, uses[0], x, []( Circuit& c, const Sum& part, const std::vector<u32>& in ) { return direct( c, part, in ); }
    );
    CHECK( error( c, out, s ) < 1e-12 );
}

// W_M^(a jk + b j + c k + d + e j^2 + f k^2), its zero powers left out.
Exponent ex( i64 a, i64 b, i64 c, i64 d, i64 e = 0, i64 f = 0 )
{
    Exponent out;
    for( const auto& [powers, v] : std::initializer_list<std::pair<std::array<u8, 2>, i64>> {
             { { 1, 1 }, a }, { { 1, 0 }, b }, { { 0, 1 }, c }, { { 0, 0 }, d }, { { 2, 0 }, e }, { { 0, 2 }, f }
         } )
    {
        if( v != 0 )
        {
            out[powers] = v;
        }
    }
    return out;
}

// cos(2pi e / M) and sin(2pi e / M) as sums of W_M^(-+e); sin's 1/(2i) is W^(M/4) / 2.
std::map<Exponent, Rational> cosine( Exponent e )
{
    Exponent neg;
    for( const auto& [p, v] : e )
    {
        neg[p] = -v;
    }
    return { { e, Rational( 1, 2 ) }, { neg, Rational( 1, 2 ) } };
}

std::map<Exponent, Rational> sine( u64 M, Exponent e )
{
    Exponent neg;
    for( const auto& [p, v] : e )
    {
        neg[p] = -v;
    }
    neg[{ 0, 0 }] += static_cast<i64>( M / 4 );
    e[{ 0, 0 }] += static_cast<i64>( M / 4 );
    return { { neg, Rational( 1, 2 ) }, { e, Rational( -1, 2 ) } };
}

Sum real_sum( u32 n, u32 m, u64 M, std::map<Exponent, Rational> c )
{
    return normalised( Sum { n, m, M, std::move( c ), true } );
}

// cos(pi j (2k + 1) / 2N): DCT-II's transpose.
TEST_CASE( "a_dct_iii_is_a_transposed_dct_ii", "[linear][sums]" )
{
    const Sum     s = real_sum( 32, 32, 128, cosine( ex( 2, 1, 0, 0 ) ) );
    const Planned p = planned( s );
    INFO( p.how << " " << p.cost.adds << "/" << p.cost.muls );
    CHECK( p.how.starts_with( "transpose(reflect(" ) );
    CHECK( p.cost.adds + p.cost.muls <= 389 ); // DCT-II-32 planned
    CHECK( error( p, s ) < 1e-12 );
}

// sin(pi (2j + 1)(k + 1) / 2N): x[j] reflected with odd j negated, W^(4 j k + 4 j) shifted to W^(4 j (k + 1)).
TEST_CASE( "a_dst_ii_reflects_and_shifts_to_a_dct_ii_s_cost", "[linear][sums]" )
{
    const Sum     s = real_sum( 32, 32, 128, sine( 128, ex( 2, 2, 1, 1 ) ) );
    const Planned p = planned( s );
    INFO( p.how << " " << p.cost.adds << "/" << p.cost.muls );
    CHECK( p.how.find( "shift 1(" ) != std::string::npos );
    CHECK( p.cost.adds + p.cost.muls <= 389 ); // DCT-II-32 planned
    CHECK( error( p, s ) < 1e-12 );
}

// cos(pi (2j + 1)(2k + 1) / 4N): reflected on both sides it is a complex DFT_N/2 twiddled, 2N lg N + N in all.
TEST_CASE( "a_dct_iv_reflects_both_sides_to_2n_lg_n_plus_n", "[linear][sums]" )
{
    const Sum     s = real_sum( 32, 32, 256, cosine( ex( 4, 2, 2, 1 ) ) );
    const Planned p = planned( s );
    INFO( p.how << " " << p.cost.adds << "/" << p.cost.muls );
    CHECK( p.cost.adds + p.cost.muls <= 2 * 32 * 5 + 32 );
    CHECK( error( p, s ) < 1e-12 );
}

// cos(pi (2j + 1 + N)(2k + 1) / 4N), 2N inputs: columns fold to N, then a DCT-IV, N more adds.
TEST_CASE( "an_mdct_folds_its_columns_to_a_dct_iv", "[linear][sums]" )
{
    const Sum     s = real_sum( 32, 16, 128, cosine( ex( 4, 2, 34, 17 ) ) );
    const Planned p = planned( s );
    INFO( p.how << " " << p.cost.adds << "/" << p.cost.muls );
    CHECK( p.how.starts_with( "fold " ) );
    CHECK( p.cost.adds + p.cost.muls <= 2 * 16 * 4 + 16 + 16 );
    CHECK( error( p, s ) < 1e-12 );
}

// Kernels outside the ones the rules were built for, known and meaningless, each planned and printed
// against its direct sum and, where there is one, the best known count: keel_tests "[sums_measure]".
// KEEL_SUM_CASES="a b" plans only those.
TEST_CASE( "the_planner_measures_unfamiliar_kernels", "[.][sums_measure]" )
{
    struct Case
    {
        std::string name;
        Sum         s;
        u32         best; // 0 when none is known
    };
    const Rational h( 1, 2 );
    const auto     sum = [&]( u32 n, u32 m, u64 M, std::map<Exponent, Rational> c, bool real = false )
    { return normalised( Sum { n, m, M, std::move( c ), real } ); };
    std::map<Exponent, Rational> cas = cosine( ex( 1, 0, 0, 0 ) );
    for( const auto& [e, c] : sine( 64, ex( 1, 0, 0, 0 ) ) )
    {
        cas[e] = cas[e] + c;
    }
    Sum dft_offset    = exponent_kernel( 64, 64, 64, 1 );
    dft_offset.offset = { { ex( 0, 0, 5, 1 ), Rational( 3, 2 ) }, { ex( 0, 0, 0, 0 ), h } };
    Sum mixed_batch   = sum( 48, 48, 48, { { ex( 1, 0, 3, 0 ), 1 }, { ex( -1, 0, -3, 0 ), 1 } }, true );
    mixed_batch.batch = 3;
    // 2N lg N - N + 1 for DCT-II/III and DST-II, 2N lg N + N for DCT-IV, 4N lg N - 6N + 8 for a complex
    // DFT, 2N lg N - 4N + 6 for a real one; others add what they add to one of those.
    const std::vector<Case> cases {
        { "dct3-64", sum( 64, 64, 256, cosine( ex( 2, 1, 0, 0 ) ), true ), 705 },
        { "dct4-64", sum( 64, 64, 512, cosine( ex( 4, 2, 2, 1 ) ), true ), 832 },
        { "dst2-64", sum( 64, 64, 256, sine( 256, ex( 2, 2, 1, 1 ) ), true ), 705 },
        { "mdct-64to32", sum( 64, 32, 256, cosine( ex( 4, 2, 66, 33 ) ), true ), 384 },
        { "dht-64", sum( 64, 64, 64, cas, true ), 518 },
        { "odd-freq-dft-64", sum( 64, 64, 128, { { ex( 2, 1, 0, 0 ), 1 } } ), 1160 + 6 * 64 },
        { "fresnel-64", sum( 64, 64, 128, { { ex( -2, 0, 0, 0, 1, 1 ), 1 } } ), 1160 + 12 * 64 },
        { "zoom-dft-64-of-192", exponent_kernel( 64, 64, 192, 1 ), 0 },
        { "chirp-z-100-to-37", exponent_kernel( 100, 37, 100, 1 ), 0 },
        { "dft-243", exponent_kernel( 243, 243, 243, 1 ), 0 },
        { "dft-105", exponent_kernel( 105, 105, 105, 1 ), 0 },
        { "dft-194", exponent_kernel( 194, 194, 194, 1 ), 0 },
        { "real-dft-243", sum( 243, 243, 243, { { ex( 1, 0, 0, 0 ), 1 } }, true ), 0 },
        { "real-dct2-100", dct_ii( 100 ), 0 },
        { "dft-256-10-bins", exponent_kernel( 256, 10, 256, 1 ), 6664 },
        { "dft-10-padded-to-256", exponent_kernel( 10, 256, 256, 1 ), 0 },
        { "dft-64-stride-3", exponent_kernel( 64, 64, 64, 3 ), 1160 },
        { "two-dfts-64", sum( 64, 64, 64, { { ex( 1, 0, 0, 0 ), 1 }, { ex( 3, 0, 0, 0 ), 1 } } ), 2 * 1160 + 128 },
        { "dft-64-plus-offset", dft_offset, 1160 + 3 },
        { "real-batch-3-48", mixed_batch, 0 },
        { "nonsense-quadratic", sum( 100, 60, 210, { { ex( 7, 0, 5, 0, 3 ), Rational( 2, 3 ) } } ), 0 },
        { "nonsense-three-terms",
          sum( 96, 96, 96, { { ex( 1, 0, 0, 0 ), 1 }, { ex( 5, 1, 0, 0 ), h }, { ex( 0, 0, 2, 0, 3 ), 3 } } ),
          0 },
        { "nonsense-square-of-sum", sum( 128, 128, 256, { { ex( 2, 0, 0, 0, 1, 1 ), Rational( 5, 7 ) } } ), 0 },
        { "nonsense-real-hermitian",
          sum( 80, 80, 120, { { ex( 3, 7, 11, 2 ), Rational( 4, 3 ) }, { ex( -3, -7, -11, -2 ), Rational( 4, 3 ) } }, true ),
          0 },
        { "nonsense-j2k", sum( 60, 60, 120, { { Exponent { { { 2, 1 }, 1 } }, 1 } } ), 0 },
    };
    const char*       only = std::getenv( "KEEL_SUM_CASES" );
    const std::string pick = only ? std::string( " " ) + only + " " : "";
    for( const Case& c : cases )
    {
        if( !pick.empty() && pick.find( " " + c.name + " " ) == std::string::npos )
        {
            continue;
        }
        INFO( c.name );
        const Cost                   naive = direct_match( c.s, {} )[0].own;
        const auto                   start = std::chrono::steady_clock::now();
        const std::optional<Planned> p     = plan_sum( c.s );
        const f64                    secs  = std::chrono::duration<f64>( std::chrono::steady_clock::now() - start ).count();
        REQUIRE( p );
        const f64 err = error( *p, c.s );
        std::cout << c.name << " | " << c.s.n << "x" << c.s.m << " W" << c.s.M << ( c.s.real ? " real" : "" ) << " | naive "
                  << naive.adds + naive.muls << " | best " << c.best << " | got " << p->cost.adds + p->cost.muls << " ("
                  << p->cost.adds << "/" << p->cost.muls << ") | " << secs << "s | err " << err << " | "
                  << p->how.substr( 0, 90 ) << "\n";
        CHECK( err < 1e-11 );
    }
}

// KEEL_SUM="n M c" plans a large kernel and prints it, "real n M c" one on real x, a fourth number m outputs, and "dct n" a
// DCT-II: keel_tests
// "[sums_large]".
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
            const bool  real = std::string( spec ).starts_with( "real" );
            std::string word;
            u32         n = 0;
            u64         M = 0, c = 0;
            if( real )
            {
                in >> word;
            }
            u32 m = 0;
            in >> n >> M >> c >> m;
            s      = exponent_kernel( n, m ? m : n, M, c );
            s.real = real;
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
