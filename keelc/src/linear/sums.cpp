// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linear/sums.h"
#include <functional>
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

// c coprime to M, or M = 1.
Kernel normalised( Kernel k )
{
    k.c %= k.M;
    const u64 g = std::gcd( k.c, k.M );
    k.M         = k.c == 0 ? 1 : k.M / g;
    k.c         = k.c == 0 ? 0 : k.c / g;
    return k;
}

Cost twiddle_cost( u64 e, u64 M )
{
    return e % M == 0 ? Cost {} : scale_cost( root( e, M ), false );
}

std::string sides( const Kernel& k )
{
    return std::to_string( k.n ) + "x" + std::to_string( k.m );
}

// The circuit of a part, pasted onto its inputs.
using Part_builder = std::function<std::vector<u32>( Circuit&, const Kernel&, const std::vector<u32>& )>;

// One way a rule takes a kernel: `count` copies of each part, and what the rule adds to them.
struct Use
{
    std::vector<std::pair<u32, Kernel>> parts;
    Cost                                own;
    std::string                         how;
    std::optional<Planned>              leaf {};
};

// A fact the planner may use. `match` gives each way it takes a kernel, its side conditions met;
// `build` makes the kernel from one of them and its parts' circuits.
struct Rule
{
    const char* name;
    const char* identity;
    std::vector<Use> ( *match )( const Kernel&, const Plan_options& );
    std::vector<u32> ( *build )( Circuit&, const Kernel&, const Use&, const std::vector<u32>&, const Part_builder& );
};

// Every product, summed in j order.
std::vector<u32> direct( Circuit& c, const Kernel& k, const std::vector<u32>& x )
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

// Every product a general one past 2^16 of them.
std::vector<Use> direct_match( const Kernel& k, const Plan_options& )
{
    Cost cost { 2 * ( k.n - 1 ) * k.m, 0 };
    if( u64 { k.n } * k.m > 1 << 16 )
    {
        cost = cost + k.n * k.m * Cost { 2, 4 };
    }
    else
    {
        for( u32 o = 0; o < k.m; ++o )
        {
            for( u32 j = 0; j < k.n; ++j )
            {
                cost = cost + twiddle_cost( k.c * j % k.M * o, k.M );
            }
        }
    }
    return { { {}, cost, "direct " + sides( k ) } };
}

std::vector<u32> direct_build( Circuit& c, const Kernel& k, const Use&, const std::vector<u32>& x, const Part_builder& )
{
    return direct( c, k, x );
}

std::vector<Use> solved_match( const Kernel& k, const Plan_options& options )
{
    if( k.n > options.leaf_max || k.m > options.leaf_max || k.M > 4 * options.leaf_max || k.n * k.m < 4 )
    {
        return {};
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
        options.cache.empty() ? std::nullopt : cached_circuit( options.cache, field, rows, options.solve );
    if( !found )
    {
        found = solve( field, rows, options.solve ).circuit;
        if( !options.cache.empty() )
        {
            cache_circuit( options.cache, field, rows, options.solve, *found );
        }
    }
    Planned leaf;
    for( const Row& r : rows )
    {
        leaf.outputs.push_back( found->outputs.at( r ) );
    }
    leaf.cost    = found->cost();
    leaf.circuit = std::move( *found );
    return { { {}, leaf.cost, "solved " + sides( k ) + " W" + std::to_string( k.M ), std::move( leaf ) } };
}

std::vector<u32> solved_build( Circuit& c, const Kernel&, const Use& use, const std::vector<u32>& x, const Part_builder& )
{
    const std::vector<u32> m = c.paste( use.leaf->circuit, x );
    std::vector<u32>       out;
    for( const u32 o : use.leaf->outputs )
    {
        out.push_back( m[o] );
    }
    return out;
}

std::vector<Use> periodic_match( const Kernel& k, const Plan_options& )
{
    if( k.n <= k.M && k.m <= k.M )
    {
        return {};
    }
    const Kernel in { std::min<u32>( k.n, static_cast<u32>( k.M ) ), std::min<u32>( k.m, static_cast<u32>( k.M ) ), k.M, k.c };
    return { { { { 1, in } }, Cost { 2 * ( k.n - in.n ), 0 }, "periodic" } };
}

std::vector<u32>
periodic_build( Circuit& c, const Kernel& k, const Use& use, const std::vector<u32>& x0, const Part_builder& part )
{
    std::vector<u32> x  = x0;
    const Kernel&    in = use.parts[0].second;
    for( u32 j = in.n; j < k.n; ++j )
    {
        x[j % in.n] = c.add( x[j % in.n], x[j] );
    }
    x.resize( in.n );
    const std::vector<u32> sub = part( c, in, x );
    std::vector<u32>       out;
    for( u32 o = 0; o < k.m; ++o )
    {
        out.push_back( sub[o % in.m] );
    }
    return out;
}

// The smallest R for each P.
std::vector<Use> split_match( const Kernel& k, const Plan_options& )
{
    std::vector<Use> uses;
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
            Cost own {};
            for( u32 j1 = 1; j1 < P; ++j1 )
            {
                for( u64 k2 = 1; k2 < R; ++k2 )
                {
                    own = own + twiddle_cost( k.c * j1 % k.M * k2, k.M );
                }
            }
            const Kernel inner { k.n / P, static_cast<u32>( R ), k.M, k.c * P % k.M };
            const Kernel outer { P, k.m / static_cast<u32>( R ), k.M, k.c * R % k.M };
            uses.push_back(
                { { { P, inner }, { static_cast<u32>( R ), outer } },
                  own,
                  "split " + std::to_string( P ) + "x" + std::to_string( k.n / P ) }
            );
            break;
        }
    }
    return uses;
}

std::vector<u32> split_build( Circuit& c, const Kernel& k, const Use& use, const std::vector<u32>& x, const Part_builder& part )
{
    const auto& [P, inner] = use.parts[0];
    const auto& [R, outer] = use.parts[1];

    std::vector<std::vector<u32>> z( P );
    for( u32 j1 = 0; j1 < P; ++j1 )
    {
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
        const std::vector<u32> y = part( c, outer, in );
        for( u32 k1 = 0; k1 < outer.m; ++k1 )
        {
            out[k2 + R * k1] = y[k1];
        }
    }
    return out;
}

const Rule rules[] = {
    { "direct", "y[k] = sum over j of x[j] W^(c j k)", direct_match, direct_build },
    { "solved", "the dense solve computes its matrix", solved_match, solved_build },
    { "periodic", "W^M = 1: columns j and j + M are equal, and so are rows k and k + M", periodic_match, periodic_build },
    { "split",
      "j = j1 + P j2, k = k2 + R k1: W^(c j k) = W^(c P j2 k2) W^(c j1 k2) W^(c R j1 k1) when c P R = 0 mod M",
      split_match,
      split_build },
};

// Every rule's uses ranked by their own cost plus their parts'; only the one chosen is built.
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
        p.outputs = choice.rule->build(
            p.circuit,
            k,
            choice.use,
            x,
            [this]( Circuit& c, const Kernel& part, const std::vector<u32>& in ) { return paste( c, part, in ); }
        );
        for( u32 o = 0; o < k.m; ++o )
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

    const Choice& choose( Kernel k )
    {
        k = normalised( k );
        if( const auto found = choices_.find( k ); found != choices_.end() )
        {
            return found->second;
        }
        Choice best;
        for( const Rule& rule : rules )
        {
            for( Use& use : rule.match( k, options_ ) )
            {
                Cost        cost = use.own;
                std::string how  = use.how;
                for( u32 i = 0; i < use.parts.size(); ++i )
                {
                    const Choice& part = choose( use.parts[i].second );
                    cost               = cost + use.parts[i].first * part.cost;
                    how += ( i == 0 ? "(" : "; " ) + part.how;
                }
                if( !use.parts.empty() )
                {
                    how += ")";
                }
                if( !best.rule || cost < best.cost )
                {
                    best = { &rule, std::move( use ), cost, std::move( how ) };
                }
            }
        }
        return choices_.emplace( k, std::move( best ) ).first->second;
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

// Largest error of the circuit's outputs against the sum, relative to the input's size.
f64 error( const Circuit& circuit, const std::vector<u32>& outputs, const Kernel& k )
{
    std::mt19937                   rng( 5 );
    std::uniform_real_distribution d( -1.0, 1.0 );
    std::vector<std::complex<f64>> x( k.n );
    for( auto& v : x )
    {
        v = { d( rng ), d( rng ) };
    }
    const std::vector<std::complex<f64>> got   = circuit.evaluate( x );
    f64                                  worst = 0;
    for( u32 o = 0; o < k.m; ++o )
    {
        std::complex<f64> want = 0;
        for( u32 j = 0; j < k.n; ++j )
        {
            want += x[j] * root( k.c * j % k.M * o % k.M, k.M );
        }
        worst = std::max( worst, std::abs( got[outputs[o]] - want ) );
    }
    return worst / std::sqrt( static_cast<f64>( k.n ) );
}

f64 error( const Planned& p, const Kernel& k )
{
    return error( p.circuit, p.outputs, k );
}

// Each rule alone, its parts summed directly, on random kernels it takes.
TEST_CASE( "every_planner_rule_computes_the_naive_sum", "[linear][sums]" )
{
    std::mt19937 rng( 11 );
    Plan_options options;
    options.leaf_max = 6;
    for( const Rule& rule : rules )
    {
        INFO( rule.name << ": " << rule.identity );
        u32 checked = 0;
        for( u32 t = 0; t < 1000 && checked < 20; ++t )
        {
            const u64    M = 1 + rng() % 16;
            const Kernel k { 1 + static_cast<u32>( rng() % 16 ), 1 + static_cast<u32>( rng() % 16 ), M, rng() % M };
            for( const Use& use : rule.match( k, options ) )
            {
                Circuit          c( false );
                std::vector<u32> x( k.n );
                for( u32 j = 0; j < k.n; ++j )
                {
                    x[j] = c.input( j );
                }
                const std::vector<u32> out = rule.build(
                    c,
                    k,
                    use,
                    x,
                    []( Circuit& c, const Kernel& part, const std::vector<u32>& in ) { return direct( c, part, in ); }
                );
                INFO( sides( k ) << " W" << k.M << "^" << k.c << " " << use.how );
                REQUIRE( out.size() == k.m );
                CHECK( error( c, out, k ) < 1e-12 );
                ++checked;
            }
        }
        CHECK( checked > 0 );
    }
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
