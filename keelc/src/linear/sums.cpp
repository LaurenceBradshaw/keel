// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linear/sums.h"
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
// y[k] = sum over j < n of x[j] W_M^(c j k), for k < m.
struct Kernel
{
    u32 n;
    u32 m;
    u64 M;
    u64 c;

    auto operator<=>( const Kernel& ) const = default;
};

// The one shape the planner takes so far.
std::optional<Kernel> kernel_of( const Sum& s )
{
    if( s.coefficient.size() != 1 || std::abs( s.coefficient.begin()->second - 1.0 ) > 1e-9 )
    {
        return std::nullopt;
    }
    const Exponent& e = s.coefficient.begin()->first;
    if( e.empty() )
    {
        return Kernel { s.n, s.m, s.M, 0 };
    }
    if( e.size() != 1 || e.begin()->first != std::array<u8, 2> { 1, 1 } || e.begin()->second < 0 )
    {
        return std::nullopt;
    }
    return Kernel { s.n, s.m, s.M, static_cast<u64>( e.begin()->second ) % s.M };
}

std::complex<f64> root( u64 e, u64 M )
{
    return std::polar( 1.0, -2 * std::numbers::pi * static_cast<f64>( e % M ) / static_cast<f64>( M ) );
}

bool cheaper( Cost b, Cost a )
{
    return std::pair { b.adds + b.muls, b.muls } < std::pair { a.adds + a.muls, a.muls };
}

// c coprime to M, or M = 1.
Kernel normalised( Kernel k )
{
    k.c %= k.M;
    const u64 g = std::gcd( k.c, k.M );
    k.M         = k.c == 0 ? 1 : k.M / g;
    k.c         = k.c == 0 ? 0 : k.c / g;
    return k;
}

Cost operator+( Cost a, Cost b )
{
    return { a.adds + b.adds, a.muls + b.muls };
}

Cost operator*( u32 k, Cost a )
{
    return { k * a.adds, k * a.muls };
}

Cost twiddle_cost( u64 e, u64 M )
{
    return e % M == 0 ? Cost {} : scale_cost( root( e, M ), false );
}

std::string sides( const Kernel& k )
{
    return std::to_string( k.n ) + "x" + std::to_string( k.m );
}

// Candidates are ranked by their parts' costs added up; only the one chosen is built.
class Planner
{
public:
    explicit Planner( const Plan_options& options )
        : options_( options )
    {
    }

    Planned build( Kernel k )
    {
        k                    = normalised( k );
        const Choice& choice = choose( k );
        Planned       p;
        p.how = choice.how;
        std::vector<u32> x( k.n );
        for( u32 j = 0; j < k.n; ++j )
        {
            x[j] = p.circuit.input( j );
        }
        switch( choice.kind )
        {
        case Kind::Direct:
            p.outputs = direct( p.circuit, k, x );
            break;
        case Kind::Solved:
        {
            const std::vector<u32> m = p.circuit.paste( choice.solved, x );
            p.outputs                = choice.solved_outputs;
            for( u32& o : p.outputs )
            {
                o = m[o];
            }
            break;
        }
        case Kind::Periodic:
            p.outputs = periodic( p.circuit, k, x );
            break;
        case Kind::Split:
            p.outputs = split( p.circuit, k, choice.P, choice.R, x );
            break;
        }
        for( u32 o = 0; o < k.m; ++o )
        {
            p.circuit.outputs[Row { Element { o } }] = p.outputs[o];
        }
        p.cost = p.circuit.cost( options_.solve.fma );
        return p;
    }

private:
    enum class Kind : u8
    {
        Direct,
        Solved,
        Periodic,
        Split,
    };

    struct Choice
    {
        Kind             kind;
        Cost             cost;
        std::string      how;
        u32              P = 0;
        u32              R = 0;
        Circuit          solved { false };
        std::vector<u32> solved_outputs {};
    };

    const Choice& choose( Kernel k )
    {
        k = normalised( k );
        if( const auto found = choices_.find( k ); found != choices_.end() )
        {
            return found->second;
        }

        Choice best;
        if( k.n > k.M || k.m > k.M )
        {
            const Kernel  in  = folded( k );
            const Choice& sub = choose( in );
            best              = {
                             .kind = Kind::Periodic, .cost = sub.cost + Cost { 2 * ( k.n - in.n ), 0 }, .how = "periodic(" + sub.how + ")"
            };
            return choices_.emplace( k, std::move( best ) ).first->second;
        }

        best                = { .kind = Kind::Direct, .cost = direct_cost( k ), .how = "direct " + sides( k ) };
        const auto consider = [&]( Choice c )
        {
            if( cheaper( c.cost, best.cost ) )
            {
                best = std::move( c );
            }
        };
        if( std::optional<Choice> s = solved( k ) )
        {
            consider( std::move( *s ) );
        }
        for( u32 P = 2; P < k.n; ++P )
        {
            if( k.n % P != 0 )
            {
                continue;
            }
            const u64 r0 = k.M / std::gcd( k.M, static_cast<u64>( P ) );
            for( u64 R = r0; R <= k.m; R += r0 )
            {
                if( k.m % R != 0 )
                {
                    continue;
                }
                const u32     Q     = k.n / P;
                const u32     S     = k.m / static_cast<u32>( R );
                const Choice& inner = choose( { Q, static_cast<u32>( R ), k.M, k.c * P % k.M } );
                const Choice& outer = choose( { P, S, k.M, k.c * R % k.M } );
                Cost          cost  = P * inner.cost + static_cast<u32>( R ) * outer.cost;
                for( u32 j1 = 1; j1 < P; ++j1 )
                {
                    for( u64 k2 = 1; k2 < R; ++k2 )
                    {
                        cost = cost + twiddle_cost( k.c * j1 % k.M * k2, k.M );
                    }
                }
                consider(
                    { .kind = Kind::Split,
                      .cost = cost,
                      .how =
                          "split " + std::to_string( P ) + "x" + std::to_string( Q ) + "(" + inner.how + "; " + outer.how + ")",
                      .P = P,
                      .R = static_cast<u32>( R ) }
                );
                break;
            }
        }
        return choices_.emplace( k, std::move( best ) ).first->second;
    }

    static Kernel folded( const Kernel& k )
    {
        return { std::min<u32>( k.n, static_cast<u32>( k.M ) ), std::min<u32>( k.m, static_cast<u32>( k.M ) ), k.M, k.c };
    }

    // Every product a general one past 2^16 of them.
    static Cost direct_cost( const Kernel& k )
    {
        Cost cost { 2 * ( k.n - 1 ) * k.m, 0 };
        if( u64 { k.n } * k.m > 1 << 16 )
        {
            return cost + k.n * k.m * Cost { 2, 4 };
        }
        for( u32 o = 0; o < k.m; ++o )
        {
            for( u32 j = 0; j < k.n; ++j )
            {
                cost = cost + twiddle_cost( k.c * j % k.M * o, k.M );
            }
        }
        return cost;
    }

    // Every product, summed in j order.
    static std::vector<u32> direct( Circuit& c, const Kernel& k, const std::vector<u32>& x )
    {
        std::vector<u32> out;
        for( u32 o = 0; o < k.m; ++o )
        {
            u32 acc = 0;
            for( u32 j = 0; j < k.n; ++j )
            {
                const u64 e    = k.c * j % k.M * o % k.M;
                const u32 term = e == 0 ? x[j] : c.scale( x[j], root( e, k.M ) );
                acc            = j == 0 ? term : c.add( acc, term );
            }
            out.push_back( acc );
        }
        return out;
    }

    std::optional<Choice> solved( const Kernel& k ) const
    {
        if( k.n > options_.leaf_max || k.m > options_.leaf_max || k.M > 4 * options_.leaf_max || k.n * k.m < 4 )
        {
            return std::nullopt;
        }
        Field            field( Ring_kind::Complex, static_cast<u32>( k.M ) );
        std::vector<Row> rows( k.m );
        for( u32 o = 0; o < k.m; ++o )
        {
            for( u32 j = 0; j < k.n; ++j )
            {
                rows[o].push_back( field.root( static_cast<u32>( k.c * j % k.M * o % k.M ) ) );
            }
        }
        std::optional<Circuit> found =
            options_.cache.empty() ? std::nullopt : cached_circuit( options_.cache, field, rows, options_.solve );
        if( !found )
        {
            found = solve( field, rows, options_.solve ).circuit;
            if( !options_.cache.empty() )
            {
                cache_circuit( options_.cache, field, rows, options_.solve, *found );
            }
        }
        Choice c {
            .kind = Kind::Solved,
            .cost = found->cost( options_.solve.fma ),
            .how  = "solved " + sides( k ) + " W" + std::to_string( k.M )
        };
        for( const Row& r : rows )
        {
            c.solved_outputs.push_back( found->outputs.at( r ) );
        }
        c.solved = std::move( *found );
        return c;
    }

    // W^M = 1: columns j and j + M are equal, and so are rows k and k + M.
    std::vector<u32> periodic( Circuit& c, const Kernel& k, std::vector<u32> x )
    {
        const Kernel in = folded( k );
        for( u32 j = in.n; j < k.n; ++j )
        {
            x[j % in.n] = c.add( x[j % in.n], x[j] );
        }
        x.resize( in.n );
        const std::vector<u32> sub = paste( c, in, x );
        std::vector<u32>       out;
        for( u32 o = 0; o < k.m; ++o )
        {
            out.push_back( sub[o % in.m] );
        }
        return out;
    }

    std::vector<u32> split( Circuit& c, const Kernel& k, u32 P, u32 R, const std::vector<u32>& x )
    {
        const u32    Q = k.n / P;
        const u32    S = k.m / R;
        const Kernel inner { Q, R, k.M, k.c * P % k.M };
        const Kernel outer { P, S, k.M, k.c * R % k.M };

        std::vector<std::vector<u32>> z( P );
        for( u32 j1 = 0; j1 < P; ++j1 )
        {
            std::vector<u32> in( Q );
            for( u32 j2 = 0; j2 < Q; ++j2 )
            {
                in[j2] = x[j1 + P * j2];
            }
            z[j1] = paste( c, inner, in );
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
            const std::vector<u32> y = paste( c, outer, in );
            for( u32 k1 = 0; k1 < S; ++k1 )
            {
                out[k2 + R * k1] = y[k1];
            }
        }
        return out;
    }

    // The sub-kernel's circuit, built once, pasted onto `x`.
    std::vector<u32> paste( Circuit& c, const Kernel& k, const std::vector<u32>& x )
    {
        const Kernel key = normalised( k );
        auto         it  = built_.find( key );
        if( it == built_.end() )
        {
            it = built_.emplace( key, build( key ) ).first;
        }
        const std::vector<u32> m = c.paste( it->second.circuit, x );
        std::vector<u32>       out;
        for( const u32 o : it->second.outputs )
        {
            out.push_back( m[o] );
        }
        return out;
    }

    const Plan_options&       options_;
    std::map<Kernel, Choice>  choices_;
    std::map<Kernel, Planned> built_;
};
} // namespace

Sum exponent_kernel( u32 n, u32 m, u64 M, u64 c )
{
    Sum s { n, m, M, {} };
    s.coefficient[c % M == 0 ? Exponent {} : Exponent { { { 1, 1 }, static_cast<i64>( c % M ) } }] = 1;
    return s;
}

std::optional<Planned> plan_sum( const Sum& sum, const Plan_options& options )
{
    const std::optional<Kernel> kernel = kernel_of( sum );
    if( !kernel )
    {
        return std::nullopt;
    }
    Planner planner( options );
    return planner.build( *kernel );
}
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>

namespace keel
{
namespace
{
Planned planned( const Kernel& k )
{
    const std::optional<Planned> p = plan_sum( exponent_kernel( k.n, k.m, k.M, k.c ) );
    REQUIRE( p );
    return *p;
}

// Largest error of the plan against the sum, relative to the input's size.
f64 error( const Planned& p, const Kernel& k )
{
    std::mt19937                   rng( 5 );
    std::uniform_real_distribution d( -1.0, 1.0 );
    std::vector<std::complex<f64>> x( k.n );
    for( auto& v : x )
    {
        v = { d( rng ), d( rng ) };
    }
    const std::vector<std::complex<f64>> got   = p.circuit.evaluate( x );
    f64                                  worst = 0;
    for( u32 o = 0; o < k.m; ++o )
    {
        std::complex<f64> want = 0;
        for( u32 j = 0; j < k.n; ++j )
        {
            want += x[j] * root( k.c * j % k.M * o % k.M, k.M );
        }
        worst = std::max( worst, std::abs( got[p.outputs[o]] - want ) );
    }
    return worst / std::sqrt( static_cast<f64>( k.n ) );
}

TEST_CASE( "a_256_point_dft_splits_to_within_1_percent_of_split_radix", "[linear][sums]" )
{
    const Kernel  k { 256, 256, 256, 1 };
    const Planned p = planned( k );
    INFO( p.how << " " << p.cost.adds << "/" << p.cost.muls );
    CHECK( p.cost.adds + p.cost.muls <= 6664 * 101 / 100 );
    CHECK( error( p, k ) < 1e-12 );
}

TEST_CASE( "a_sum_with_no_rule_is_not_planned", "[linear][sums]" )
{
    Sum s                         = exponent_kernel( 96, 96, 96, 1 );
    s.coefficient.begin()->second = 2;
    CHECK( !plan_sum( s ) );
    CHECK( !plan_sum( Sum { 96, 96, 96, { { Exponent { { { 2, 1 }, 1 } }, 1.0 } } } ) );
}

TEST_CASE( "a_kernel_longer_than_its_period_folds_its_columns", "[linear][sums]" )
{
    const Kernel  k { 24, 16, 8, 3 };
    const Planned p = planned( k );
    INFO( p.how );
    CHECK( error( p, k ) < 1e-12 );
}

// KEEL_SUM="n M c" plans a large kernel and prints it: keel_tests "[sums_large]".
TEST_CASE( "a_large_kernel_is_planned", "[.][sums_large]" )
{
    const char* spec = std::getenv( "KEEL_SUM" );
    Kernel      k { 4096, 4096, 4096, 1 };
    if( spec )
    {
        std::istringstream s( spec );
        s >> k.n >> k.M >> k.c;
        k.m = k.n;
    }
    const auto    start = std::chrono::steady_clock::now();
    const Planned p     = planned( k );
    const f64     secs  = std::chrono::duration<f64>( std::chrono::steady_clock::now() - start ).count();
    const f64     err   = error( p, k );
    std::cout << "sum " << k.n << ": " << p.cost.adds << "/" << p.cost.muls << " plan " << secs << "s err " << err << "\n"
              << p.how << "\n";
    CHECK( err < 1e-11 );
}
} // namespace
} // namespace keel
#endif // ENABLE_UNIT_TESTS
