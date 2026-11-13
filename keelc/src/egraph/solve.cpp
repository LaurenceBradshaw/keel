// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "egraph/solve.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include "egraph/extract.h"
#include "egraph/sum_graph.h"

namespace keel
{
namespace
{
constexpr u32 k_none = UINT32_MAX;

// A division the ring cannot do (an even divisor in Z/2^w): the candidate needing it fails.
struct Cannot
{
};

struct Result
{
    Cost                           cost;
    std::shared_ptr<const Circuit> circuit;
};

using Maybe = std::optional<Result>;

// `b` is cheaper than `a`: by total, then by multiplies.
bool improves( const Maybe& a, const Maybe& b )
{
    const auto key = []( Cost c ) { return std::pair { c.adds + c.muls, c.muls }; };
    return b && ( !a || key( b->cost ) < key( a->cost ) );
}

// Exact small rationals, for Toom's weights.
struct Fraction
{
    i64 n = 0;
    i64 d = 1;
};

Fraction fraction( i64 n, i64 d )
{
    if( d < 0 )
    {
        n = -n;
        d = -d;
    }
    const i64 g = std::gcd( n, d );
    return { n / g, d / g };
}

Fraction operator*( Fraction a, Fraction b )
{
    return fraction( a.n * b.n, a.d * b.d );
}

Fraction operator+( Fraction a, Fraction b )
{
    return fraction( a.n * b.d + b.n * a.d, a.d * b.d );
}

i64 power( i64 b, u32 e )
{
    i64 r = 1;
    while( e-- > 0 )
    {
        r *= b;
    }
    return r;
}

std::vector<i64> product( const std::vector<i64>& a, const std::vector<i64>& b )
{
    std::vector<i64> p( a.size() + b.size() - 1, 0 );
    for( size_t i = 0; i < a.size(); ++i )
    {
        for( size_t j = 0; j < b.size(); ++j )
        {
            p[i + j] += a[i] * b[j];
        }
    }
    return p;
}

bool nonzero( const Row& r, Element zero )
{
    return std::ranges::any_of( r, [&]( Element e ) { return e != zero; } );
}

size_t distinct_count( std::vector<Element> v )
{
    std::ranges::sort( v );
    return static_cast<size_t>( std::ranges::unique( v ).begin() - v.begin() );
}

// The points q/p of Toom's evaluations, as factors p y - q.
constexpr std::pair<i64, i64> k_toom_points[] = { { 1, 0 }, { 1, 1 }, { 1, -1 }, { 1, 2 }, { 1, -2 }, { 2, 1 }, { 2, -1 } };

// M[ro[i]][co[j]] * r[i] * c[j] is the canonical map.
struct Canon
{
    std::vector<Row>     map;
    std::vector<u32>     ro, co;
    std::vector<Element> r, c;
};

struct Rotation
{
    u32     j, k;
    Element p, q, r, s;
};

// Bijections onto Z_n with M[i][j] = f(r_i + c_j).
struct Hankel
{
    std::vector<u32> r, c;
    Row              f;
};

// The sum of the route's inputs and its outputs.
using Routed = std::optional<std::pair<u32, std::vector<u32>>>;

class Solver
{
public:
    Solver( Field& field, const Solve_options& options )
        : options_( options ),
          F_( &field )
    {
        fields_.emplace( field.n(), &field );
        base_ = field.spent();
    }

    Solution run( const std::vector<Row>& given );

private:
    using Blocks = std::vector<std::vector<u32>>;
    using Route  = Routed ( Solver::* )( Circuit&, const std::vector<u32>&, const Row&, u32 );

    struct Factored
    {
        std::vector<std::vector<Row>> firsts;
        std::vector<Row>              s;
        size_t                        pieces = 0;
    };

    struct Memo
    {
        Maybe              got;
        std::vector<Row>   rows;
        Canon              canon;
        std::optional<i64> had; // the fuel it had, when it ran short
    };

    Field&  field( u32 n );
    u64     spent() const;
    i64     left();
    Maybe   within( i64 share, const std::function<Maybe()>& f );
    Circuit circuit() const;
    u32     times( Circuit& c, u32 a, Element e ) const;
    Element divide( Element a, Element b ) const;
    Element rational( Fraction q ) const;
    Result  finish( const Circuit& c ) const;
    Result  naive( const std::vector<Row>& rows ) const;

    Maybe solve( const std::vector<Row>& rows );
    Maybe solve_in( u32 n, const std::vector<Row>& rows );
    Maybe solve_map( const std::vector<Row>& rows );
    Canon canon( const std::vector<Row>& rows ) const;
    Maybe direct( const std::vector<Row>& rows );

    std::vector<std::vector<u32>> components( const std::vector<Row>& rows ) const;
    std::vector<Blocks>           partitions( const std::vector<Row>& rows ) const;
    Factored                      factor( const std::vector<Row>& rows, const Blocks& part ) const;
    Maybe                         via_components( const std::vector<Row>& rows, const std::vector<std::vector<u32>>& groups );
    Maybe                         via_blocks( const std::vector<Row>& rows, const Blocks& part, const Factored& f );
    Maybe                         via_diagonal( const std::vector<Row>& rows, const Row& d );
    size_t                        consts( const std::vector<Row>& rows ) const;
    std::optional<std::vector<Rotation>> rotations( const std::vector<Row>& rows, u32 r1, u32 r2 ) const;
    std::pair<u32, u32>                  rotate( Circuit& c, u32 x, u32 y, Rotation b ) const;
    Maybe                                via_rotations( const std::vector<Row>& rows, const std::vector<Rotation>& blocks );
    std::optional<Hankel>                hankel( const std::vector<Row>& rows ) const;
    std::vector<std::pair<u32, u32>>     peels( const std::vector<Row>& rows ) const;
    std::optional<std::vector<u32>>      stage( Circuit& c, const std::vector<Row>& m, const std::vector<u32>& ins );
    Routed                               dft_route( Circuit& c, const std::vector<u32>& u, const Row& f, u32 extra );
    Routed                               crt_route( Circuit& c, const std::vector<u32>& u, const Row& f, u32 extra );
    Maybe                                via_product( const std::vector<Row>& md, u32 d );
    Maybe via_convolution( const std::vector<Row>& rows, u32 rb, u32 cb, const Hankel& h, Route route );
    std::optional<std::pair<i64, Row>> toeplitz( const std::vector<Row>& rows ) const;
    std::vector<Row>                   linconv( const Row& g, size_t q ) const;
    Maybe                              via_toom( const std::vector<Row>& rows, i64 dlo, Row g, u32 k );

    const Solve_options&                                            options_;
    Field*                                                          F_;
    std::map<u32, Field*>                                           fields_;
    std::vector<std::unique_ptr<Field>>                             owned_;
    std::map<std::pair<u32, std::vector<Row>>, std::optional<Memo>> memo_; // empty while in progress
    u64                                                             base_ = 0;
    u64                                                             own_  = 0; // work outside the fields
    std::vector<i64>                                                ceiling_ { INT64_C( 1 ) << 62 };
    bool                                                            shares_  = false;
    bool                                                            ran_out_ = false;
    u64                                                             starved_ = 0; // times a ceiling was met or a search clipped
    u32                                                             reused_  = 0;
};

Field& Solver::field( u32 n )
{
    if( const auto found = fields_.find( n ); found != fields_.end() )
    {
        return *found->second;
    }
    owned_.push_back( std::make_unique<Field>( F_->kind(), n ) );
    fields_.emplace( n, owned_.back().get() );
    return *owned_.back();
}

u64 Solver::spent() const
{
    u64 s = own_;
    for( const auto& [n, f] : fields_ )
    {
        s += f->spent();
    }
    return s - base_;
}

i64 Solver::left()
{
    const i64 l = ceiling_.back() - static_cast<i64>( spent() );
    if( l <= 0 )
    {
        ran_out_ = true;
        ++starved_;
    }
    return l;
}

// The second pass holds each candidate to its share of what is left.
Maybe Solver::within( i64 share, const std::function<Maybe()>& f )
{
    if( !shares_ )
    {
        return f();
    }
    ceiling_.push_back( std::min( ceiling_.back(), static_cast<i64>( spent() ) + share ) );
    try
    {
        Maybe r = f();
        ceiling_.pop_back();
        return r;
    }
    catch( ... )
    {
        ceiling_.pop_back();
        throw;
    }
}

Circuit Solver::circuit() const
{
    return F_->kind() == Ring_kind::Integer ? Circuit( *F_ ) : Circuit( F_->kind() == Ring_kind::Real );
}

u32 Solver::times( Circuit& c, u32 a, Element e ) const
{
    return F_->kind() == Ring_kind::Integer ? c.scale( a, e ) : c.scale( a, F_->value( e ) );
}

Element Solver::divide( Element a, Element b ) const
{
    if( !F_->invertible( b ) )
    {
        throw Cannot {};
    }
    return F_->div( a, b );
}

Element Solver::rational( Fraction q ) const
{
    if( F_->kind() == Ring_kind::Integer && q.d % 2 == 0 )
    {
        throw Cannot {};
    }
    return F_->rational( q.n, q.d );
}

Result Solver::finish( const Circuit& c ) const
{
    Circuit p    = c.pruned();
    Cost    cost = p.cost();
    return { cost, std::make_shared<const Circuit>( std::move( p ) ) };
}

// The input program: each output a chain of scaled inputs, products shared.
Result Solver::naive( const std::vector<Row>& rows ) const
{
    Circuit c = circuit();
    for( const Row& r : rows )
    {
        u32 acc = k_none;
        for( u32 j = 0; j < r.size(); ++j )
        {
            if( r[j] != F_->zero() )
            {
                const u32 t = times( c, c.input( j ), r[j] );
                acc         = acc == k_none ? t : c.add( acc, t );
            }
        }
        if( acc != k_none )
        {
            c.outputs[r] = acc;
        }
    }
    return finish( c );
}

Maybe Solver::direct( const std::vector<Row>& rows )
{
    if( left() <= 0 || rows.size() > 64 || rows[0].size() > 64 )
    {
        return std::nullopt;
    }
    const u64 room  = static_cast<u64>( left() );
    const u64 fuel  = std::min( options_.graph_fuel, room );
    const u64 dfuel = std::max<u64>( 1, std::min( options_.restart_fuel, room / options_.restarts ) );
    starved_ += fuel < options_.graph_fuel || dfuel < options_.restart_fuel;
    try
    {
        const u64       before = F_->spent();
        const Sum_graph graph( *F_, rows, fuel );
        own_ += graph.spent() - ( F_->spent() - before );
        std::optional<Extraction> x = extract( graph, rows, { options_.restarts, 1, dfuel } );
        if( !x )
        {
            return std::nullopt;
        }
        own_ += x->spent;
        return finish( x->circuit );
    }
    catch( const Field::Overflow& )
    {
        return std::nullopt;
    }
}

std::vector<std::vector<u32>> Solver::components( const std::vector<Row>& rows ) const
{
    const u32        n = static_cast<u32>( rows[0].size() );
    std::vector<u32> parent( n );
    std::iota( parent.begin(), parent.end(), 0 );
    const auto find = [&]( u32 x )
    {
        while( parent[x] != x )
        {
            parent[x] = parent[parent[x]];
            x         = parent[x];
        }
        return x;
    };
    for( const Row& r : rows )
    {
        u32 first = k_none;
        for( u32 j = 0; j < n; ++j )
        {
            if( r[j] == F_->zero() )
            {
                continue;
            }
            if( first == k_none )
            {
                first = j;
            }
            else
            {
                parent[find( j )] = find( first );
            }
        }
    }
    std::vector<std::vector<u32>> groups;
    std::map<u32, size_t>         at;
    for( u32 j = 0; j < n; ++j )
    {
        const auto [found, added] = at.try_emplace( find( j ), groups.size() );
        if( added )
        {
            groups.emplace_back();
        }
        groups[found->second].push_back( j );
    }
    return groups;
}

// The inputs grouped by the ratio of two full rows, each grouping once.
std::vector<Solver::Blocks> Solver::partitions( const std::vector<Row>& rows ) const
{
    const size_t        n = rows[0].size();
    std::vector<Blocks> found;
    std::set<Blocks>    seen;
    const auto          full = [&]( const Row& r )
    { return std::ranges::all_of( r, [&]( Element e ) { return F_->invertible( e ); } ); };
    for( size_t a = 0; a < rows.size(); ++a )
    {
        if( !full( rows[a] ) )
        {
            continue;
        }
        for( size_t b = a + 1; b < rows.size(); ++b )
        {
            if( std::ranges::find( rows[b], F_->zero() ) != rows[b].end() )
            {
                continue;
            }
            Blocks                    part;
            std::map<Element, size_t> at;
            for( u32 j = 0; j < n; ++j )
            {
                const auto [it, added] = at.try_emplace( F_->div( rows[b][j], rows[a][j] ), part.size() );
                if( added )
                {
                    part.emplace_back();
                }
                part[it->second].push_back( j );
            }
            std::ranges::sort( part );
            if( part.size() > 1 && part.size() < n && seen.insert( part ).second )
            {
                found.push_back( std::move( part ) );
            }
        }
    }
    return found;
}

// Per block, the distinct restricted rows up to a scalar, normalised at the reference input that
// leaves the second stage the fewest distinct constants.
Solver::Factored Solver::factor( const std::vector<Row>& rows, const Blocks& part ) const
{
    Factored out;
    out.s.resize( rows.size() );
    for( const std::vector<u32>& blk : part )
    {
        struct Choice
        {
            size_t                               consts;
            std::vector<Row>                     shapes;
            std::vector<std::pair<u32, Element>> col; // shape and scalar, k_none for a zero row
        };
        std::optional<Choice> best;
        for( const u32 ref : blk )
        {
            Choice               c { 0, {}, {} };
            std::map<Row, u32>   index;
            std::vector<Element> scalars;
            for( const Row& r : rows )
            {
                u32 at = r[ref] != F_->zero() ? ref : k_none;
                for( size_t i = 0; at == k_none && i < blk.size(); ++i )
                {
                    at = r[blk[i]] != F_->zero() ? blk[i] : k_none;
                }
                if( at == k_none )
                {
                    c.col.emplace_back( k_none, F_->zero() );
                    continue;
                }
                Row shape;
                for( const u32 j : blk )
                {
                    shape.push_back( divide( r[j], r[at] ) );
                }
                const auto [it, added] = index.try_emplace( shape, static_cast<u32>( c.shapes.size() ) );
                if( added )
                {
                    c.shapes.push_back( std::move( shape ) );
                }
                c.col.emplace_back( it->second, r[at] );
                scalars.push_back( F_->orbit( r[at] ).first );
            }
            c.consts = distinct_count( scalars );
            if( !best || c.consts < best->consts )
            {
                best = std::move( c );
            }
        }
        out.pieces += best->shapes.size();
        for( size_t i = 0; i < rows.size(); ++i )
        {
            Row row( best->shapes.size(), F_->zero() );
            if( best->col[i].first != k_none )
            {
                row[best->col[i].first] = best->col[i].second;
            }
            out.s[i].insert( out.s[i].end(), row.begin(), row.end() );
        }
        out.firsts.push_back( std::move( best->shapes ) );
    }
    return out;
}

Maybe Solver::via_components( const std::vector<Row>& rows, const std::vector<std::vector<u32>>& groups )
{
    Circuit c = circuit();
    for( const std::vector<u32>& cols : groups )
    {
        std::vector<Row> sub;
        for( const Row& r : rows )
        {
            Row k;
            for( const u32 j : cols )
            {
                k.push_back( r[j] );
            }
            if( nonzero( k, F_->zero() ) )
            {
                sub.push_back( std::move( k ) );
            }
        }
        const Maybe sc = solve( sub );
        if( !sc )
        {
            return std::nullopt;
        }
        std::vector<u32> ins;
        for( const u32 j : cols )
        {
            ins.push_back( c.input( j ) );
        }
        const std::vector<u32> m = c.paste( *sc->circuit, ins );
        std::set<Row>          done; // a repeated row takes each part once
        for( const Row& r : rows )
        {
            if( !done.insert( r ).second )
            {
                continue;
            }
            Row k;
            for( const u32 j : cols )
            {
                k.push_back( r[j] );
            }
            const auto part = sc->circuit->outputs.find( k );
            if( part == sc->circuit->outputs.end() )
            {
                continue;
            }
            const auto have = c.outputs.find( r );
            c.outputs[r]    = have == c.outputs.end() ? m[part->second] : c.add( have->second, m[part->second] );
        }
    }
    return finish( c );
}

// The block maps to pieces, then the pieces to outputs.
Maybe Solver::via_blocks( const std::vector<Row>& rows, const Blocks& part, const Factored& f )
{
    Circuit          c = circuit();
    std::vector<u32> pv;
    for( size_t b = 0; b < part.size(); ++b )
    {
        if( part[b].size() == 1 )
        {
            pv.push_back( c.input( part[b][0] ) );
            continue;
        }
        const Maybe fc = solve( f.firsts[b] );
        if( !fc )
        {
            return std::nullopt;
        }
        std::vector<u32> ins;
        for( const u32 j : part[b] )
        {
            ins.push_back( c.input( j ) );
        }
        const std::vector<u32> m = c.paste( *fc->circuit, ins );
        for( const Row& s : f.firsts[b] )
        {
            pv.push_back( m[fc->circuit->outputs.at( s )] );
        }
    }
    const Maybe sc = solve( f.s );
    if( !sc )
    {
        return std::nullopt;
    }
    const std::vector<u32> m = c.paste( *sc->circuit, pv );
    for( size_t i = 0; i < rows.size(); ++i )
    {
        c.outputs[rows[i]] = m[sc->circuit->outputs.at( f.s[i] )];
    }
    return finish( c );
}

// M = M' diag(d), so the row of M that is d is all ones in M'.
Maybe Solver::via_diagonal( const std::vector<Row>& rows, const Row& d )
{
    std::vector<Row> m2;
    for( const Row& r : rows )
    {
        Row x;
        for( size_t j = 0; j < r.size(); ++j )
        {
            x.push_back( divide( r[j], d[j] ) );
        }
        m2.push_back( std::move( x ) );
    }
    const Maybe sc = solve( m2 );
    if( !sc )
    {
        return std::nullopt;
    }
    Circuit          c = circuit();
    std::vector<u32> ins;
    for( u32 j = 0; j < d.size(); ++j )
    {
        ins.push_back( times( c, c.input( j ), d[j] ) );
    }
    const std::vector<u32> m = c.paste( *sc->circuit, ins );
    for( size_t i = 0; i < rows.size(); ++i )
    {
        c.outputs[rows[i]] = m[sc->circuit->outputs.at( m2[i] )];
    }
    return finish( c );
}

size_t Solver::consts( const std::vector<Row>& rows ) const
{
    std::vector<Element> reps;
    for( const Row& r : rows )
    {
        for( const Element e : r )
        {
            if( e != F_->zero() && !F_->is_unit( e ) )
            {
                reps.push_back( F_->orbit( e ).first );
            }
        }
    }
    return distinct_count( std::move( reps ) );
}

// Every input paired so its 2x2 at rows r1, r2 is a rotation or a reflection; greedy, so it can miss
// a full matching that exists.
std::optional<std::vector<Rotation>> Solver::rotations( const std::vector<Row>& rows, u32 r1, u32 r2 ) const
{
    const u32 n = static_cast<u32>( rows[0].size() );
    if( n % 2 != 0 )
    {
        return std::nullopt;
    }
    std::vector<u32> free( n );
    std::iota( free.begin(), free.end(), 0 );
    std::vector<Rotation> blocks;
    while( !free.empty() )
    {
        const u32 j = free.front();
        free.erase( free.begin() );
        bool paired = false;
        for( size_t i = 0; i < free.size() && !paired; ++i )
        {
            const u32     k = free[i];
            const Element p = rows[r1][j], q = rows[r1][k], r = rows[r2][j], s = rows[r2][k];
            const bool    zero  = p == F_->zero() || q == F_->zero() || r == F_->zero() || s == F_->zero();
            const bool    units = F_->is_unit( p ) && F_->is_unit( q ) && F_->is_unit( r ) && F_->is_unit( s );
            if( zero || units ) // butterflies are found elsewhere
            {
                continue;
            }
            if( ( p == s && q == F_->neg( r ) ) || ( p == F_->neg( s ) && q == r ) )
            {
                blocks.push_back( { j, k, p, q, r, s } );
                free.erase( free.begin() + static_cast<std::ptrdiff_t>( i ) );
                paired = true;
            }
        }
        if( !paired )
        {
            return std::nullopt;
        }
    }
    return blocks;
}

// (p x + q y, r x + s y): Gauss's 3 multiplies and 3 adds, or 2 and 2 when |p| = |q|.
std::pair<u32, u32> Solver::rotate( Circuit& c, u32 x, u32 y, Rotation b ) const
{
    const bool flip = b.p == F_->neg( b.s ); // a reflection: the rotation with its second row negated
    if( flip )
    {
        b.r = F_->neg( b.r );
        b.s = F_->neg( b.s );
    }
    u32 u, v;
    if( F_->orbit( b.p ).first == F_->orbit( b.q ).first )
    {
        u = c.add( times( c, x, b.p ), times( c, y, b.q ) );
        v = c.add( times( c, x, b.r ), times( c, y, b.s ) );
    }
    else // p x + q y = t - (p - q) y, r x + p y = t + (r - p) x, t = p (x + y)
    {
        const u32 t = times( c, c.add( x, y ), b.p );
        u           = c.add( t, c.scale( y, F_->value( b.q ) - F_->value( b.p ) ) );
        v           = c.add( t, c.scale( x, F_->value( b.r ) - F_->value( b.p ) ) );
    }
    return { u, flip ? c.scale( v, -1.0 ) : v };
}

// M = M' B, B the blocks on input pairs; only a rotation that simplifies the map is explored.
Maybe Solver::via_rotations( const std::vector<Row>& rows, const std::vector<Rotation>& blocks )
{
    std::vector<Row> m2 = rows;
    Circuit          c  = circuit();
    std::vector<u32> u;
    for( u32 j = 0; j < rows[0].size(); ++j )
    {
        u.push_back( c.input( j ) );
    }
    for( const Rotation& b : blocks )
    {
        const Element det = F_->add( F_->mul( b.p, b.s ), F_->neg( F_->mul( b.q, b.r ) ) );
        for( Row& row : m2 ) // (a, b) B^-1 = (a s - b r, b p - a q) / det
        {
            const Element x = row[b.j], y = row[b.k];
            row[b.j] = divide( F_->add( F_->mul( x, b.s ), F_->neg( F_->mul( y, b.r ) ) ), det );
            row[b.k] = divide( F_->add( F_->mul( y, b.p ), F_->neg( F_->mul( x, b.q ) ) ), det );
        }
        std::tie( u[b.j], u[b.k] ) = rotate( c, u[b.j], u[b.k], b );
    }
    if( consts( m2 ) >= consts( rows ) )
    {
        return std::nullopt;
    }
    const Maybe sc = solve( m2 );
    if( !sc )
    {
        return std::nullopt;
    }
    const std::vector<u32> m = c.paste( *sc->circuit, u );
    for( size_t i = 0; i < rows.size(); ++i )
    {
        c.outputs[rows[i]] = m[sc->circuit->outputs.at( m2[i] )];
    }
    return finish( c );
}

// Needs f injective: row i1 against row 0 is a shift of the labels, and a shift coprime to n names
// them all.
std::optional<Hankel> Solver::hankel( const std::vector<Row>& rows ) const
{
    const size_t n = rows.size();
    if( n < 2 || rows[0].size() != n )
    {
        return std::nullopt;
    }
    Row first = rows[0];
    std::ranges::sort( first );
    for( const Row& r : rows )
    {
        Row sorted = r;
        std::ranges::sort( sorted );
        if( std::ranges::find( r, F_->zero() ) != r.end() || std::ranges::adjacent_find( sorted ) != sorted.end() ||
            sorted != first )
        {
            return std::nullopt;
        }
    }
    for( size_t i1 = 1; i1 < n; ++i1 )
    {
        std::map<Element, Element> sig;
        for( size_t j = 0; j < n; ++j )
        {
            sig.emplace( rows[0][j], rows[i1][j] );
        }
        Row f { rows[0][0] };
        while( f.size() < n )
        {
            f.push_back( sig.at( f.back() ) );
        }
        if( distinct_count( f ) < n )
        {
            continue;
        }
        std::map<Element, u32> lab;
        for( u32 t = 0; t < n; ++t )
        {
            lab.emplace( f[t], t );
        }
        Hankel h { {}, {}, f };
        for( const Element v : rows[0] )
        {
            h.c.push_back( lab.at( v ) );
        }
        for( const Row& row : rows )
        {
            h.r.push_back( static_cast<u32>( ( lab.at( row[0] ) + n - h.c[0] ) % n ) );
        }
        for( size_t i = 0; i < n; ++i )
        {
            for( size_t j = 0; j < n; ++j )
            {
                if( rows[i][j] != f[( h.r[i] + h.c[j] ) % n] )
                {
                    return std::nullopt;
                }
            }
        }
        return h;
    }
    return std::nullopt;
}

// The row and column to set aside so the rest may be Hankel, k_none to keep all.
std::vector<std::pair<u32, u32>> Solver::peels( const std::vector<Row>& rows ) const
{
    std::vector<u32> bad_r, bad_c;
    for( u32 i = 0; i < rows.size(); ++i )
    {
        if( distinct_count( rows[i] ) < rows[i].size() || std::ranges::find( rows[i], F_->zero() ) != rows[i].end() )
        {
            bad_r.push_back( i );
        }
    }
    for( u32 j = 0; j < rows[0].size(); ++j )
    {
        Row col;
        for( const Row& r : rows )
        {
            col.push_back( r[j] );
        }
        if( distinct_count( col ) < rows.size() || std::ranges::find( col, F_->zero() ) != col.end() )
        {
            bad_c.push_back( j );
        }
    }
    if( bad_r.size() > 1 || bad_c.size() > 1 ) // the rest would still hold a repeated row or column
    {
        return {};
    }
    if( bad_r.empty() )
    {
        bad_r.push_back( k_none );
    }
    if( bad_c.empty() )
    {
        bad_c.push_back( k_none );
    }
    std::vector<std::pair<u32, u32>> out;
    for( const u32 rb : bad_r )
    {
        for( const u32 cb : bad_c )
        {
            out.emplace_back( rb, cb );
        }
    }
    return out;
}

// The nodes for each row of `m` applied to `ins`, k_none for a zero input or row; empty if unsolved.
std::optional<std::vector<u32>> Solver::stage( Circuit& c, const std::vector<Row>& m, const std::vector<u32>& ins )
{
    std::vector<u32> live;
    for( u32 j = 0; j < ins.size(); ++j )
    {
        if( ins[j] != k_none && std::ranges::any_of( m, [&]( const Row& r ) { return r[j] != F_->zero(); } ) )
        {
            live.push_back( j );
        }
    }
    std::vector<Row> sub, keep;
    for( const Row& r : m )
    {
        Row s;
        for( const u32 j : live )
        {
            s.push_back( r[j] );
        }
        if( nonzero( s, F_->zero() ) )
        {
            keep.push_back( s );
        }
        sub.push_back( std::move( s ) );
    }
    if( keep.empty() )
    {
        return std::vector<u32>( m.size(), k_none );
    }
    const Maybe sc = solve( keep );
    if( !sc )
    {
        return std::nullopt;
    }
    std::vector<u32> bound;
    for( const u32 j : live )
    {
        bound.push_back( ins[j] );
    }
    const std::vector<u32> nodes = c.paste( *sc->circuit, bound );
    std::vector<u32>       out;
    for( const Row& s : sub )
    {
        out.push_back( nonzero( s, F_->zero() ) ? nodes[sc->circuit->outputs.at( s )] : k_none );
    }
    return out;
}

// y_t = sum_m f(t+m) u_m by the convolution theorem with F = DFT_n: y_t = (F V)_{-t},
// V_k = (F f)_k / n * (F u)_{-k}.
Routed Solver::dft_route( Circuit& c, const std::vector<u32>& u, const Row& f, u32 extra )
{
    const u32                      n = static_cast<u32>( f.size() );
    std::vector<std::complex<f64>> fhat( n );
    for( u32 k = 0; k < n; ++k )
    {
        for( u32 s = 0; s < n; ++s )
        {
            fhat[k] += F_->value( f[s] ) * std::polar( 1.0, -2 * std::numbers::pi * k * s / n );
        }
        fhat[k] /= n;
    }
    Field&           g = field( n );
    std::vector<Row> dft( n );
    for( u32 k = 0; k < n; ++k )
    {
        for( u32 m = 0; m < n; ++m )
        {
            dft[k].push_back( g.root( k * m % n ) );
        }
    }
    const Maybe dc = solve_in( n, dft );
    if( !dc )
    {
        return std::nullopt;
    }
    const std::vector<u32> m1 = c.paste( *dc->circuit, u );
    std::vector<u32>       big_u, v;
    for( const Row& d : dft )
    {
        big_u.push_back( m1[dc->circuit->outputs.at( d )] );
    }
    for( u32 k = 0; k < n; ++k )
    {
        v.push_back( c.scale( big_u[( n - k ) % n], fhat[k] ) );
    }
    if( extra != k_none ) // every row of F has 1 on V_0
    {
        v[0] = c.add( v[0], extra );
    }
    const std::vector<u32> m2 = c.paste( *dc->circuit, v );
    std::vector<u32>       y;
    for( u32 t = 0; t < n; ++t )
    {
        y.push_back( m2[dc->circuit->outputs.at( dft[( n - t ) % n] )] );
    }
    return std::pair { big_u[0], y };
}

// Winograd over Q: y(x) = f(x) w(x) mod x^n - 1 with w_s = u_{-s}, x^n - 1 the product of Phi_d over
// d | n. Residues w_d = w mod Phi_d; v_d = (T_d f) w_d mod Phi_d, T_d = Q_d^-1 mod Phi_d,
// Q_d = (x^n - 1) / Phi_d; then y = sum_d Q_d v_d. Each block v_d is the cheaper of its own sub-map
// and the product g w_d reduced mod Phi_d.
Routed Solver::crt_route( Circuit& c, const std::vector<u32>& u, const Row& f, u32 extra )
{
    const u32                                     n = static_cast<u32>( f.size() );
    std::vector<Row>                              a, b( n );
    std::vector<std::pair<std::vector<Row>, u32>> blocks;
    for( u32 d = 1; d <= n; ++d )
    {
        if( n % d != 0 )
        {
            continue;
        }
        Field&                  g   = field( d );
        const std::vector<i64>& phi = g.phi();
        const u32               deg = g.degree();
        std::vector<i64>        q, num( n + 1, 0 );
        num[0] = -1;
        num[n] = 1;
        for( u32 i = n + 1; i-- > deg; ) // Phi_d is monic
        {
            const i64 x = num[i];
            q.insert( q.begin(), x );
            for( u32 k = 0; k <= deg; ++k )
            {
                num[i - deg + k] -= x * phi[k];
            }
        }
        const Element t = g.inv( g.polynomial( q ) );
        for( u32 i = 0; i < deg; ++i )
        {
            Row row;
            for( u32 s = 0; s < n; ++s )
            {
                const auto [p, den] = g.coefficient( g.root( s ), i );
                row.push_back( rational( fraction( p, den ) ) );
            }
            a.push_back( std::move( row ) );
        }
        for( u32 s = 0; s < n; ++s )
        {
            for( u32 i = 0; i < deg; ++i )
            {
                b[s].push_back( s >= i && s - i < q.size() ? F_->rational( q[s - i] ) : F_->zero() );
            }
        }
        std::vector<Element> xt; // x^e T mod Phi_d
        for( u32 e = 0; e < 2 * n; ++e )
        {
            xt.push_back( g.mul( g.root( e ), t ) );
        }
        std::vector<Row> md( deg, Row( deg, F_->zero() ) ); // (x^k T f mod Phi_d)_i
        for( u32 i = 0; i < deg; ++i )
        {
            for( u32 k = 0; k < deg; ++k )
            {
                for( u32 j = 0; j < n; ++j )
                {
                    const auto [p, den] = g.coefficient( xt[j + k], i );
                    if( p != 0 )
                    {
                        md[i][k] = F_->add( md[i][k], F_->mul( F_->rational( p, den ), f[j] ) );
                    }
                }
            }
        }
        blocks.emplace_back( std::move( md ), d );
    }

    std::vector<u32> w;
    for( u32 s = 0; s < n; ++s )
    {
        w.push_back( u[( n - s ) % n] );
    }
    const std::optional<std::vector<u32>> big_u = stage( c, a, w );
    if( !big_u )
    {
        return std::nullopt;
    }
    std::vector<u32> v;
    size_t           at = 0;
    for( const auto& [md, d] : blocks )
    {
        const std::vector<u32> ins(
            big_u->begin() + static_cast<std::ptrdiff_t>( at ), big_u->begin() + static_cast<std::ptrdiff_t>( at + md.size() )
        );
        at += md.size();
        if( std::ranges::none_of( md, [&]( const Row& r ) { return nonzero( r, F_->zero() ); } ) ) // f = 0 mod Phi_d
        {
            v.insert( v.end(), md.size(), k_none );
            continue;
        }
        Maybe best = solve( md );
        if( md.size() > 1 )
        {
            if( Maybe p = via_product( md, d ); improves( best, p ) )
            {
                best = std::move( p );
            }
        }
        if( !best )
        {
            return std::nullopt;
        }
        const std::vector<u32> m = c.paste( *best->circuit, ins );
        for( const Row& row : md )
        {
            v.push_back( m[best->circuit->outputs.at( row )] );
        }
    }
    if( extra != k_none ) // Q_1 is all ones
    {
        v[0] = v[0] == k_none ? extra : c.add( v[0], extra );
    }
    const std::optional<std::vector<u32>> y = stage( c, b, v );
    if( !y || std::ranges::find( *y, k_none ) != y->end() )
    {
        return std::nullopt;
    }
    return std::pair { ( *big_u )[0], *y };
}

// `md` multiplies by g (its column 0) in Q[x]/Phi_d: the product g r, then mod Phi_d.
Maybe Solver::via_product( const std::vector<Row>& md, u32 d )
{
    const u32 m = static_cast<u32>( md.size() );
    Field&    g = field( d );
    Circuit   c = circuit();
    Row       col;
    for( const Row& row : md )
    {
        col.push_back( row[0] );
    }
    std::vector<u32> ins;
    for( u32 k = 0; k < m; ++k )
    {
        ins.push_back( c.input( k ) );
    }
    const std::optional<std::vector<u32>> p = stage( c, linconv( col, m ), ins );
    if( !p )
    {
        return std::nullopt;
    }
    std::vector<Row> reduce;
    for( u32 t = 0; t < m; ++t )
    {
        Row row;
        for( u32 e = 0; e < 2 * m - 1; ++e )
        {
            const auto [num, den] = g.coefficient( g.root( e % d ), t );
            row.push_back( rational( fraction( num, den ) ) );
        }
        reduce.push_back( std::move( row ) );
    }
    const std::optional<std::vector<u32>> v = stage( c, reduce, *p );
    if( !v || std::ranges::find( *v, k_none ) != v->end() )
    {
        return std::nullopt;
    }
    for( u32 i = 0; i < m; ++i )
    {
        c.outputs[md[i]] = ( *v )[i];
    }
    return finish( c );
}

// The core (rows without rb, columns without cb) is Hankel: y_r = sum_m f(r+m) u_m, a cyclic
// correlation. A peeled column constant over the core rows joins the route's V_0; a peeled row
// constant over the core columns is that constant times the route's sum of u.
Maybe Solver::via_convolution( const std::vector<Row>& rows, u32 rb, u32 cb, const Hankel& h, Route route )
{
    const u32        n = static_cast<u32>( h.f.size() );
    std::vector<u32> core_cols, core_rows;
    for( u32 j = 0; j < rows[0].size(); ++j )
    {
        if( j != cb )
        {
            core_cols.push_back( j );
        }
    }
    for( u32 i = 0; i < rows.size(); ++i )
    {
        if( i != rb )
        {
            core_rows.push_back( i );
        }
    }
    std::vector<u32> inv( n );
    for( u32 p = 0; p < n; ++p )
    {
        inv[h.c[p]] = core_cols[p];
    }
    Circuit    c    = circuit();
    const auto plus = [&]( u32 acc, u32 j, Element e )
    {
        if( e == F_->zero() )
        {
            return acc;
        }
        const u32 t = times( c, c.input( j ), e );
        return acc == k_none ? t : c.add( acc, t );
    };
    u32  extra  = k_none;
    bool folded = false; // the peeled column went into V_0
    if( cb != k_none )
    {
        Row col;
        for( const u32 i : core_rows )
        {
            col.push_back( rows[i][cb] );
        }
        if( distinct_count( col ) == 1 && col[0] != F_->zero() )
        {
            extra  = times( c, c.input( cb ), col[0] );
            folded = true;
        }
    }
    std::vector<u32> u;
    for( u32 m = 0; m < n; ++m )
    {
        u.push_back( c.input( inv[m] ) );
    }
    const Routed got = ( this->*route )( c, u, h.f, extra );
    if( !got )
    {
        return std::nullopt;
    }
    const auto& [u0, y] = *got;
    for( u32 p = 0; p < core_rows.size(); ++p )
    {
        const Row& row = rows[core_rows[p]];
        c.outputs[row] = cb == k_none || folded ? y[h.r[p]] : plus( y[h.r[p]], cb, row[cb] );
    }
    if( rb != k_none )
    {
        Row part;
        for( const u32 j : core_cols )
        {
            part.push_back( rows[rb][j] );
        }
        u32 acc = k_none;
        if( distinct_count( part ) == 1 && part[0] != F_->zero() ) // a constant row is c * sum u
        {
            acc = times( c, u0, part[0] );
        }
        else
        {
            for( u32 p = 0; p < core_cols.size(); ++p )
            {
                acc = plus( acc, core_cols[p], part[p] );
            }
        }
        c.outputs[rows[rb]] = cb == k_none ? acc : plus( acc, cb, rows[rb][cb] );
    }
    return finish( c );
}

// M[i][j] = t(i - j): the least nonzero offset and t over the band from it, or empty.
std::optional<std::pair<i64, Row>> Solver::toeplitz( const std::vector<Row>& rows ) const
{
    if( rows.size() < 2 || rows[0].size() < 2 )
    {
        return std::nullopt;
    }
    std::map<i64, Element> t;
    for( size_t i = 0; i < rows.size(); ++i )
    {
        for( size_t j = 0; j < rows[i].size(); ++j )
        {
            const auto [it, added] = t.try_emplace( static_cast<i64>( i ) - static_cast<i64>( j ), rows[i][j] );
            if( it->second != rows[i][j] )
            {
                return std::nullopt;
            }
        }
    }
    std::vector<i64> nz;
    for( const auto& [d, e] : t )
    {
        if( e != F_->zero() )
        {
            nz.push_back( d );
        }
    }
    if( nz.size() < 2 )
    {
        return std::nullopt;
    }
    Row g;
    for( i64 d = nz.front(); d <= nz.back(); ++d )
    {
        g.push_back( t.at( d ) );
    }
    return std::pair { nz.front(), g };
}

// The rows of the linear convolution by g on q inputs.
std::vector<Row> Solver::linconv( const Row& g, size_t q ) const
{
    std::vector<Row> rows( g.size() + q - 1, Row( q, F_->zero() ) );
    for( size_t i = 0; i < rows.size(); ++i )
    {
        for( size_t j = 0; j < q && j <= i; ++j )
        {
            if( i - j < g.size() )
            {
                rows[i][j] = g[i - j];
            }
        }
    }
    return rows;
}

// Row i is (g * r)_(i - dlo). Toom-k: g and r in k pieces of h, as polynomials in y = x^h; the
// product at 2k - 2 points and infinity, each a linear convolution of h; P(y) = sum_i P(a_i) M_i(y) /
// M_i(a_i) + P_inf M(y) / lead, M = prod (p y - q), M_i = M / (p_i y - q_i). The scalars go to the g
// side, so evaluation (scaled by p^(k-1)) and interpolation stay integer. k = 2 is Karatsuba.
Maybe Solver::via_toom( const std::vector<Row>& rows, i64 dlo, Row g, u32 k )
{
    const size_t q   = rows[0].size();
    const size_t h   = ( std::max( g.size(), q ) + k - 1 ) / k;
    const auto   pts = std::span( k_toom_points ).first( 2 * k - 2 );
    g.resize( k * h, F_->zero() );
    i64              lead = 1;
    std::vector<i64> big_m { 1 };
    for( const auto [p, s] : pts )
    {
        big_m = product( big_m, { -s, p } );
        lead *= p;
    }

    std::vector<std::vector<i64>> polys; // per product: interpolation polynomial, g side, r-side rows
    std::vector<Row>              gs, e;
    for( size_t i = 0; i <= pts.size(); ++i )
    {
        std::vector<i64> mi { 1 }, coef;
        Fraction         w;
        if( i < pts.size() )
        {
            const auto [p, s] = pts[i];
            for( size_t i2 = 0; i2 < pts.size(); ++i2 )
            {
                if( i2 != i )
                {
                    mi = product( mi, { -pts[i2].second, pts[i2].first } );
                }
            }
            Fraction at;
            for( size_t x = 0; x < mi.size(); ++x )
            {
                at = at + fraction( mi[x] * power( s, static_cast<u32>( x ) ), power( p, static_cast<u32>( x ) ) );
            }
            at = at * fraction( power( p, 2 * k - 2 ), 1 );
            w  = fraction( at.d, at.n );
            for( u32 a = 0; a < k; ++a )
            {
                coef.push_back( power( s, a ) * power( p, k - 1 - a ) );
            }
        }
        else
        {
            mi = big_m;
            w  = fraction( 1, lead );
            coef.assign( k, 0 );
            coef.back() = 1;
        }
        polys.push_back( mi );
        const Element weight = rational( w );
        Row           gi;
        for( size_t t = 0; t < h; ++t )
        {
            Element acc = F_->zero();
            for( u32 a = 0; a < k; ++a )
            {
                acc = F_->add( acc, F_->mul( F_->rational( coef[a] ), g[a * h + t] ) );
            }
            gi.push_back( F_->mul( acc, weight ) );
        }
        gs.push_back( std::move( gi ) );
        for( size_t t = 0; t < h; ++t )
        {
            Row row( q, F_->zero() );
            for( size_t j = 0; j < q; ++j )
            {
                if( j % h == t )
                {
                    row[j] = F_->rational( coef[j / h] );
                }
            }
            e.push_back( std::move( row ) );
        }
    }

    Circuit          c = circuit();
    std::vector<u32> ins;
    for( u32 j = 0; j < q; ++j )
    {
        ins.push_back( c.input( j ) );
    }
    const std::optional<std::vector<u32>> r = stage( c, e, ins );
    if( !r )
    {
        return std::nullopt;
    }
    std::vector<u32> big_p;
    for( size_t i = 0; i < gs.size(); ++i )
    {
        const std::vector<u32> part(
            r->begin() + static_cast<std::ptrdiff_t>( i * h ), r->begin() + static_cast<std::ptrdiff_t>( ( i + 1 ) * h )
        );
        const std::optional<std::vector<u32>> pi = stage( c, linconv( gs[i], h ), part );
        if( !pi )
        {
            return std::nullopt;
        }
        big_p.insert( big_p.end(), pi->begin(), pi->end() );
    }
    std::vector<Row> interp; // x^(h e + t) of the product: sum over products i of M_i[e] P_i[t]
    for( size_t i = 0; i < rows.size(); ++i )
    {
        const i64 z = static_cast<i64>( i ) - dlo;
        Row       row;
        for( const std::vector<i64>& mi : polys )
        {
            for( size_t t = 0; t < 2 * h - 1; ++t )
            {
                const i64  x  = z - static_cast<i64>( t );
                const i64  hh = static_cast<i64>( h );
                const bool on = x % hh == 0 && x >= 0 && x / hh < static_cast<i64>( mi.size() );
                row.push_back( on ? F_->rational( mi[static_cast<size_t>( x / hh )] ) : F_->zero() );
            }
        }
        interp.push_back( std::move( row ) );
    }
    const std::optional<std::vector<u32>> y = stage( c, interp, big_p );
    if( !y || std::ranges::find( *y, k_none ) != y->end() )
    {
        return std::nullopt;
    }
    for( size_t i = 0; i < rows.size(); ++i )
    {
        c.outputs[rows[i]] = ( *y )[i];
    }
    return finish( c );
}

// The map up to row and column order and free units on rows and columns. Signatures (entries up
// to units) are refined until the classes stop splitting; ties are then broken by content until the
// order is stable. Not a full canonical form: two equal maps may still differ, a memo miss but never
// a wrong reuse.
Canon Solver::canon( const std::vector<Row>& rows ) const
{
    const size_t                      m = rows.size(), n = rows[0].size();
    std::vector<std::vector<Element>> u( m, std::vector<Element>( n ) );
    for( size_t i = 0; i < m; ++i )
    {
        for( size_t j = 0; j < n; ++j )
        {
            u[i][j] = F_->orbit( rows[i][j] ).first;
        }
    }
    using Signature  = std::vector<std::pair<u32, Element>>;
    const auto ranks = []( const std::vector<Signature>& sig )
    {
        std::vector<Signature> sorted = sig;
        std::ranges::sort( sorted );
        sorted.erase( std::ranges::unique( sorted ).begin(), sorted.end() );
        std::vector<u32> r;
        for( const Signature& s : sig )
        {
            r.push_back( static_cast<u32>( std::ranges::lower_bound( sorted, s ) - sorted.begin() ) );
        }
        return std::pair { r, sorted.size() };
    };
    std::vector<u32> rk( m, 0 ), ck( n, 0 );
    size_t           rk_count = 1, ck_count = 1;
    while( true ) // each round splits classes or stops: at most m + n rounds
    {
        std::vector<Signature> rs( m ), cs( n );
        for( size_t i = 0; i < m; ++i )
        {
            for( size_t j = 0; j < n; ++j )
            {
                rs[i].emplace_back( ck[j], u[i][j] );
                cs[j].emplace_back( rk[i], u[i][j] );
            }
        }
        for( Signature& s : rs )
        {
            std::ranges::sort( s );
        }
        for( Signature& s : cs )
        {
            std::ranges::sort( s );
        }
        auto [rk2, rc] = ranks( rs );
        auto [ck2, cc] = ranks( cs );
        if( rc == rk_count && cc == ck_count )
        {
            break;
        }
        rk       = std::move( rk2 );
        ck       = std::move( ck2 );
        rk_count = rc;
        ck_count = cc;
    }

    // The unit making the first nonzero entry its orbit's representative, and v times it.
    const auto normed = [&]( const Row& v )
    {
        const auto first = std::ranges::find_if( v, [&]( Element e ) { return e != F_->zero(); } );
        if( first == v.end() )
        {
            return std::pair { F_->one(), v };
        }
        const Element s = F_->orbit( *first ).second;
        Row           out;
        for( const Element x : v )
        {
            out.push_back( F_->mul( x, s ) );
        }
        return std::pair { s, out };
    };
    std::vector<u32> ro( m ), co( n );
    std::iota( ro.begin(), ro.end(), 0 );
    std::iota( co.begin(), co.end(), 0 );
    // Stable sorts, ties kept in their current order (libstdc++'s stable_sort takes a deprecated buffer).
    const auto sort_by = []( std::vector<u32>& order, const auto& less )
    {
        std::vector<u32> at( order.size() );
        for( u32 p = 0; p < order.size(); ++p )
        {
            at[order[p]] = p;
        }
        std::ranges::sort( order, [&]( u32 a, u32 b ) { return less( a, b ) || ( !less( b, a ) && at[a] < at[b] ); } );
    };
    sort_by( ro, [&]( u32 a, u32 b ) { return rk[a] < rk[b]; } );
    sort_by( co, [&]( u32 a, u32 b ) { return ck[a] < ck[b]; } );
    for( size_t round = 0; round < m + n; ++round ) // ties by content: rows, then columns
    {
        const std::vector<u32> last_ro = ro, last_co = co;
        std::vector<Row>       row_key( m );
        std::vector<Element>   unit( m );
        for( const u32 i : ro )
        {
            Row v;
            for( const u32 j : co )
            {
                v.push_back( rows[i][j] );
            }
            std::tie( unit[i], row_key[i] ) = normed( v );
        }
        sort_by( ro, [&]( u32 a, u32 b ) { return std::tie( rk[a], row_key[a] ) < std::tie( rk[b], row_key[b] ); } );
        std::vector<Row> col_key( n );
        for( const u32 j : co )
        {
            Row v;
            for( const u32 i : ro )
            {
                v.push_back( F_->mul( rows[i][j], unit[i] ) );
            }
            col_key[j] = normed( v ).second;
        }
        sort_by( co, [&]( u32 a, u32 b ) { return std::tie( ck[a], col_key[a] ) < std::tie( ck[b], col_key[b] ); } );
        if( ro == last_ro && co == last_co )
        {
            break;
        }
    }

    Canon out { {}, ro, co, std::vector<Element>( m, F_->one() ), std::vector<Element>( n, F_->one() ) };
    for( const u32 i : ro )
    {
        Row v;
        for( const u32 j : co )
        {
            v.push_back( rows[i][j] );
        }
        out.map.push_back( std::move( v ) );
    }
    for( size_t a = 0; a < m; ++a ) // each row's first entry, then each column's
    {
        std::tie( out.r[a], out.map[a] ) = normed( out.map[a] );
    }
    for( size_t b = 0; b < n; ++b )
    {
        Row col;
        for( size_t a = 0; a < m; ++a )
        {
            col.push_back( out.map[a][b] );
        }
        std::tie( out.c[b], col ) = normed( col );
        for( size_t a = 0; a < m; ++a )
        {
            out.map[a][b] = col[a];
        }
    }
    return out;
}

// Memoised by the canonical map. An entry solved short of fuel is reused only by a caller with no
// more; one in progress is a cycle, not a solution. Candidates are found on the rows as given: unit
// scaling would hide a Hankel core.
Maybe Solver::solve( const std::vector<Row>& rows )
{
    Canon cn = canon( rows );
    own_ += rows.size() * rows[0].size(); // the canonical form, charged like a sub-map solved
    std::pair<u32, std::vector<Row>> key { F_->n(), cn.map };
    if( const auto found = memo_.find( key ); found != memo_.end() )
    {
        if( !found->second )
        {
            return std::nullopt;
        }
        const Memo& e = *found->second;
        if( !e.had || left() <= *e.had )
        {
            if( e.rows == rows || !e.got )
            {
                return e.got;
            }
            // M[ro a][co b] = M0[ro0 a][co0 b] * (r0 / r)[a] * (c0 / c)[b]
            ++reused_;
            const Circuit& sc = *e.got->circuit;
            own_ += sc.ops().size(); // the paste
            Circuit          out = circuit();
            std::vector<u32> ins( cn.co.size() );
            for( size_t b = 0; b < cn.co.size(); ++b )
            {
                ins[e.canon.co[b]] = times( out, out.input( cn.co[b] ), F_->div( e.canon.c[b], cn.c[b] ) );
            }
            const std::vector<u32> m = out.paste( sc, ins );
            for( size_t a = 0; a < cn.ro.size(); ++a )
            {
                out.outputs[rows[cn.ro[a]]] =
                    times( out, m[sc.outputs.at( e.rows[e.canon.ro[a]] )], F_->div( e.canon.r[a], cn.r[a] ) );
            }
            return finish( out );
        }
    }
    if( left() <= 0 )
    {
        return naive( rows );
    }
    const u64 hungry = starved_;
    const i64 had    = left();
    memo_[key]       = std::nullopt;
    Maybe best;
    try
    {
        best = solve_map( rows );
    }
    catch( ... )
    {
        memo_.erase( key );
        throw;
    }
    memo_[key] = Memo { best, rows, std::move( cn ), starved_ == hungry ? std::optional<i64> {} : std::optional { had } };
    return best;
}

Maybe Solver::solve_in( u32 n, const std::vector<Row>& rows )
{
    Field* const old = F_;
    F_               = &field( n );
    try
    {
        Maybe r = solve( rows );
        F_      = old;
        return r;
    }
    catch( ... )
    {
        F_ = old;
        throw;
    }
}

// The input program, then each candidate on an equal share of what is left, unused fuel passing on.
// A candidate that overflows or needs a division the ring lacks is dropped.
Maybe Solver::solve_map( const std::vector<Row>& rows )
{
    own_ += rows.size() * rows[0].size();
    const size_t                        n       = rows[0].size();
    Maybe                               best    = naive( rows );
    const auto                          groups  = components( rows );
    const bool                          real    = F_->kind() == Ring_kind::Real;
    const bool                          integer = F_->kind() == Ring_kind::Integer;
    std::vector<std::function<Maybe()>> cands; // an empty one is the direct search
    const auto                          guarded = [&]( const std::function<void()>& f )
    {
        try
        {
            f();
        }
        catch( const Field::Overflow& )
        {
        }
        catch( const Cannot& )
        {
        }
    };
    if( groups.size() > 1 )
    {
        cands.emplace_back( [&] { return via_components( rows, groups ); } );
    }
    else
    {
        guarded(
            [&]
            {
                for( const Blocks& part : partitions( rows ) )
                {
                    guarded(
                        [&]
                        {
                            Factored f      = factor( rows, part );
                            size_t   firsts = 0;
                            for( const auto& x : f.firsts )
                            {
                                firsts += x.size();
                            }
                            if( firsts < rows.size() * part.size() ) // a piece is shared
                            {
                                cands.emplace_back( [&, part, f = std::move( f )] { return via_blocks( rows, part, f ); } );
                            }
                        }
                    );
                }
            }
        );
        for( const Row& r : rows ) // a full row as the diagonal
        {
            const bool full  = std::ranges::all_of( r, [&]( Element e ) { return e != F_->zero() && F_->invertible( e ); } );
            const bool units = std::ranges::all_of( r, [&]( Element e ) { return F_->is_unit( e ); } );
            if( full && !units )
            {
                cands.emplace_back( [&, r] { return via_diagonal( rows, r ); } );
            }
        }
        if( real ) // complex: a rotation is a constant
        {
            std::set<std::vector<std::tuple<u32, u32, Element, Element, Element>>> seen;
            for( u32 r1 = 0; r1 < rows.size(); ++r1 )
            {
                for( u32 r2 = r1 + 1; r2 < rows.size(); ++r2 )
                {
                    guarded(
                        [&]
                        {
                            const auto b = rotations( rows, r1, r2 );
                            if( !b )
                            {
                                return;
                            }
                            std::vector<std::tuple<u32, u32, Element, Element, Element>> key; // B up to scale
                            for( const Rotation& x : *b )
                            {
                                key.emplace_back( x.j, x.k, F_->div( x.q, x.p ), F_->div( x.r, x.p ), F_->div( x.s, x.p ) );
                            }
                            if( seen.insert( key ).second )
                            {
                                cands.emplace_back( [&, b = *b] { return via_rotations( rows, b ); } );
                            }
                        }
                    );
                }
            }
        }
        bool convolution = false;
        for( const auto [rb, cb] : integer ? std::vector<std::pair<u32, u32>> {} : peels( rows ) )
        {
            std::vector<Row> core;
            for( u32 i = 0; i < rows.size(); ++i )
            {
                if( i == rb )
                {
                    continue;
                }
                Row row;
                for( u32 j = 0; j < n; ++j )
                {
                    if( j != cb )
                    {
                        row.push_back( rows[i][j] );
                    }
                }
                core.push_back( std::move( row ) );
            }
            const std::optional<Hankel> h = hankel( core );
            if( !h )
            {
                continue;
            }
            if( !real ) // real data: the complex DFT route is not costed
            {
                cands.emplace_back( [&, rb, cb, h = *h] { return via_convolution( rows, rb, cb, h, &Solver::dft_route ); } );
            }
            cands.emplace_back( [&, rb, cb, h = *h] { return via_convolution( rows, rb, cb, h, &Solver::crt_route ); } );
            convolution = true;
            break;
        }
        if( const auto t = convolution ? std::optional<std::pair<i64, Row>> {} : toeplitz( rows ) ) // a cyclic map goes by CRT
        {
            for( const u32 k : { 2u, 3u } )
            {
                if( k <= std::max( t->second.size(), n ) )
                {
                    cands.emplace_back( [&, k, t = *t] { return via_toom( rows, t.first, t.second, k ); } );
                }
            }
        }
        const bool scales = std::ranges::all_of(
            rows, [&]( const Row& r ) { return std::ranges::count_if( r, [&]( Element e ) { return e != F_->zero(); } ) == 1; }
        );
        if( !scales ) // scales only: the input is the circuit
        {
            cands.emplace_back();
        }
    }

    bool won = false;
    for( size_t k = 0; k < cands.size(); ++k )
    {
        if( left() <= 0 )
        {
            break;
        }
        const i64 share = left() / static_cast<i64>( cands.size() - k );
        Maybe     c;
        try
        {
            if( !cands[k] )
            {
                if( won && std::max( n, rows.size() ) > options_.direct_max )
                {
                    continue;
                }
                c = within( share, [&] { return direct( rows ); } );
            }
            else
            {
                c = within( share, cands[k] );
            }
        }
        catch( const Field::Overflow& )
        {
        }
        catch( const Cannot& )
        {
        }
        if( improves( best, c ) )
        {
            best = std::move( c );
            won  = true;
        }
    }
    return best;
}

Solution Solver::run( const std::vector<Row>& given )
{
    // The outputs up to units: the first copy of each is solved as given, since units hide a Hankel core.
    std::vector<Row>                                       rows;
    std::map<Row, std::pair<size_t, Element>>              seen;
    std::vector<std::optional<std::pair<size_t, Element>>> of; // r = rows[i] * unit, empty for 0
    for( const Row& r : given )
    {
        const auto first = std::ranges::find_if( r, [&]( Element e ) { return e != F_->zero(); } );
        if( first == r.end() )
        {
            of.emplace_back();
            continue;
        }
        const Element s = F_->orbit( *first ).second;
        Row           k;
        for( const Element e : r )
        {
            k.push_back( F_->mul( e, s ) );
        }
        const auto [it, added] = seen.try_emplace( k, rows.size(), s );
        if( added )
        {
            rows.push_back( r );
        }
        of.emplace_back( std::pair { it->second.first, F_->div( it->second.second, s ) } );
    }

    Solution out { circuit(), {}, naive( given ).cost, 0, 0, 0 };
    if( rows.empty() )
    {
        return out;
    }
    if( options_.fuel != 0 )
    {
        ceiling_[0] = static_cast<i64>( options_.fuel );
    }
    Maybe got = solve( rows );
    if( ran_out_ && options_.fuel != 0 ) // pass 1 spent depth-first and ran out: shares now
    {
        const Maybe first = got;
        memo_.clear();
        shares_     = true;
        ceiling_[0] = static_cast<i64>( spent() + options_.fuel );
        got         = solve( rows );
        if( !improves( first, got ) )
        {
            got = first;
        }
    }
    assert( got );

    out.circuit = *got->circuit;
    for( size_t i = 0; i < given.size(); ++i )
    {
        if( of[i] )
        {
            const u32 node                = out.circuit.outputs.at( rows[of[i]->first] );
            out.circuit.outputs[given[i]] = times( out.circuit, node, of[i]->second );
        }
    }
    out.cost   = out.circuit.cost();
    out.spent  = spent();
    out.memo   = static_cast<u32>( memo_.size() );
    out.reused = reused_;
    return out;
}
} // namespace

Solution solve( Field& field, const std::vector<Row>& rows, const Solve_options& options )
{
    return Solver( field, options ).run( rows );
}
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace keel
{
namespace
{
const std::filesystem::path k_specs = std::filesystem::path( __FILE__ ).parent_path() / "../../test/egraph";

// A spec entry: k is W^k, -1 is zero, else a sum of terms q@k, q W^k, with q a rational.
Element entry( Field& field, const std::string& t )
{
    if( t.find( '@' ) == std::string::npos )
    {
        const i64 k = std::stoll( t );
        return k < 0 ? field.zero() : field.root( static_cast<u32>( k ) );
    }
    Element            v = field.zero();
    std::istringstream terms( t );
    for( std::string term; std::getline( terms, term, '+' ); )
    {
        const size_t at = term.find( '@' );
        const size_t sl = term.find( '/' );
        const i64    q  = std::stoll( term.substr( 0, std::min( at, sl ) ) );
        const i64    d  = sl < at ? std::stoll( term.substr( sl + 1, at - sl - 1 ) ) : 1;
        v               = field.add(
            v, field.mul( field.rational( q, d ), field.root( static_cast<u32>( std::stoll( term.substr( at + 1 ) ) ) ) )
        );
    }
    return v;
}

struct Spec
{
    std::unique_ptr<Field> field;
    std::vector<Row>       rows;
};

// Lines `N n`, `n inputs`, `R` for a real map, and `t entries...` per row.
Spec read_spec( const std::string& name )
{
    std::ifstream            in( k_specs / "specs" / ( name + ".spec" ) );
    std::vector<std::string> rows;
    u32                      n    = 0;
    bool                     real = false;
    for( std::string line; std::getline( in, line ); )
    {
        std::istringstream s( line );
        std::string        tag;
        s >> tag;
        if( tag == "N" )
        {
            s >> n;
        }
        else if( tag == "R" )
        {
            real = true;
        }
        else if( tag == "t" )
        {
            rows.push_back( line.substr( 2 ) );
        }
    }
    Spec spec { std::make_unique<Field>( real ? Ring_kind::Real : Ring_kind::Complex, n ), {} };
    for( const std::string& line : rows )
    {
        std::istringstream s( line );
        Row                row;
        for( std::string t; s >> t; )
        {
            row.push_back( entry( *spec.field, t ) );
        }
        spec.rows.push_back( std::move( row ) );
    }
    return spec;
}

// Each map's recorded adds/muls, by name.
std::map<std::string, Cost> expected()
{
    std::ifstream               in( k_specs / "expected.txt" );
    std::map<std::string, Cost> out;
    for( std::string line; std::getline( in, line ); )
    {
        std::istringstream s( line );
        std::string        name, cost;
        s >> name >> cost;
        const size_t slash = cost.find( '/' );
        out[name]          = {
            static_cast<u32>( std::stoul( cost.substr( 0, slash ) ) ),
            static_cast<u32>( std::stoul( cost.substr( slash + 1 ) ) )
        };
    }
    return out;
}

bool no_worse( Cost got, Cost want )
{
    return std::pair { got.adds + got.muls, got.muls } <= std::pair { want.adds + want.muls, want.muls };
}

// The circuit computes every row on random input.
bool computes( const Circuit& c, const Field& field, const std::vector<Row>& rows )
{
    std::mt19937                   rng( 5 );
    std::vector<std::complex<f64>> x;
    for( size_t j = 0; j < rows[0].size(); ++j )
    {
        const f64 re = static_cast<f64>( rng() ) / 4294967296.0;
        const f64 im = static_cast<f64>( rng() ) / 4294967296.0;
        x.emplace_back( re, field.kind() == Ring_kind::Complex ? im : 0 );
    }
    const std::vector<std::complex<f64>> v = c.evaluate( x );
    for( const Row& r : rows )
    {
        std::complex<f64> want = 0;
        for( size_t j = 0; j < r.size(); ++j )
        {
            want += field.value( r[j] ) * x[j];
        }
        const auto o = c.outputs.find( r );
        if( o == c.outputs.end() ? std::abs( want ) > 1e-9 : std::abs( want - v[o->second] ) > 1e-9 * ( 1 + std::abs( want ) ) )
        {
            return false;
        }
    }
    return true;
}

void check_specs( const std::vector<std::string>& names )
{
    const std::map<std::string, Cost> want = expected();
    for( const std::string& name : names )
    {
        INFO( name );
        Spec           spec = read_spec( name );
        const Solution got  = solve( *spec.field, spec.rows );
        INFO( got.cost.adds << "/" << got.cost.muls << " against " << want.at( name ).adds << "/" << want.at( name ).muls );
        CHECK( no_worse( got.cost, want.at( name ) ) );
        CHECK( no_worse( got.cost, got.input ) );
        CHECK( computes( got.circuit, *spec.field, spec.rows ) );
    }
}
} // namespace

TEST_CASE( "solve_gives_the_recorded_costs", "[egraph][solve]" )
{
    check_specs( { "dft3", "dft8", "rdft4", "rdft5", "rdft8", "rhaar16", "dct4", "circ4", "lin4" } );
}

// Every recorded map: keel_tests "[specs]".
TEST_CASE( "solve_gives_every_recorded_cost", "[.][specs]" )
{
    std::vector<std::string> names;
    for( const auto& [name, cost] : expected() )
    {
        names.push_back( name );
    }
    check_specs( names );
}

TEST_CASE( "solve_gives_one_circuit", "[egraph][solve]" )
{
    Spec a = read_spec( "dft8" );
    Spec b = read_spec( "dft8" );
    CHECK( solve( *a.field, a.rows ).circuit.text() == solve( *b.field, b.rows ).circuit.text() );
}

TEST_CASE( "solve_keeps_zero_and_unit_multiple_rows", "[egraph][solve]" )
{
    Field            field( Ring_kind::Complex, 4 );
    const Element    z = field.zero(), one = field.one(), i = field.root( 1 );
    std::vector<Row> rows { { one, one, one }, { z, z, z }, { i, i, i }, { one, field.minus_one(), z } };
    const Solution   got = solve( field, rows );
    CHECK( got.circuit.outputs.size() == 3 );
    CHECK( got.cost == Cost { 6, 0 } );
    CHECK( computes( got.circuit, field, rows ) );
}

TEST_CASE( "solve_integers_without_halves", "[egraph][solve]" )
{
    // WHT4 and a 3-tap FIR over 3 inputs in Z/2^32, which has no 1/2: no butterflies, no Toom-3.
    Field            field( Ring_kind::Integer, 1, 32 );
    std::vector<Row> wht( 4 );
    for( u32 r = 0; r < 4; ++r )
    {
        for( u32 c = 0; c < 4; ++c )
        {
            wht[r].push_back( std::popcount( r & c ) % 2 ? field.minus_one() : field.one() );
        }
    }
    const Solution w = solve( field, wht );
    CHECK( w.cost == Cost { 8, 0 } );
    CHECK( computes( w.circuit, field, wht ) );

    std::vector<Row> fir;
    for( i64 i = 0; i < 5; ++i )
    {
        Row row;
        for( i64 j = 0; j < 3; ++j )
        {
            row.push_back( i - j >= 0 && i - j < 3 ? field.rational( 3 + 2 * ( i - j ) ) : field.zero() );
        }
        fir.push_back( std::move( row ) );
    }
    const Solution f = solve( field, fir );
    CHECK( no_worse( f.cost, f.input ) );
    CHECK( computes( f.circuit, field, fir ) );
}
} // namespace keel
#endif // ENABLE_UNIT_TESTS
