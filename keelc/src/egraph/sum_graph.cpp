// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "egraph/sum_graph.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <optional>

namespace keel
{
namespace
{
// Each class with at least 3 inputs and no active part is split by its 2 largest active sub-sums.
constexpr u32 k_contained = 2;

u32 count( u64 s )
{
    return static_cast<u32>( std::popcount( s ) );
}

u32 first( u64 s )
{
    return static_cast<u32>( std::countr_zero( s ) );
}
} // namespace

size_t Sum_graph::Form_hash::operator()( const Form& f ) const
{
    u64 h = 1469598103934665603ull;
    for( u32 j = 0; j < n; ++j )
    {
        h = ( h ^ f[j].v ) * 0x100000001b3ull;
    }
    return h ^ h >> 29;
}

size_t Sum_graph::Node_key_hash::operator()( const Node_key& x ) const
{
    const u64 h = ( u64 { x.c } << 32 | x.a ) * 0x9E3779B97F4A7C15ull ^
                  ( u64 { x.b } << 20 ^ u64 { x.k } << 2 ^ u64( x.kind ) ) * 0xC2B2AE3D27D4EB4Full;
    return h ^ h >> 29;
}

u64 Sum_graph::spent() const
{
    return steps_ + field_->spent() - base_;
}

bool Sum_graph::burnt()
{
    if( fuel_ != 0 && spent() > fuel_ )
    {
        out_of_fuel_ = true;
    }
    return out_of_fuel_;
}

Element Sum_graph::lead( const Form& f, u64 s ) const
{
    return s ? f[first( s )] : field_->one();
}

Sum_graph::Form Sum_graph::scaled( Form f, Element c )
{
    for( u32 j = 0; j < n_; ++j )
    {
        if( f[j] != field_->zero() )
        {
            f[j] = field_->mul( c, f[j] );
        }
    }
    return f;
}

Sum_graph::Form Sum_graph::restricted( Form f, u64 mask ) const
{
    for( u32 j = 0; j < n_; ++j )
    {
        if( !( mask >> j & 1 ) )
        {
            f[j] = field_->zero();
        }
    }
    return f;
}

u64 Sum_graph::support( const Form& f ) const
{
    u64 s = 0;
    for( u32 j = 0; j < n_; ++j )
    {
        if( f[j] != field_->zero() )
        {
            s |= 1ull << j;
        }
    }
    return s;
}

// The distinct constants `f` needs, up to free units: over the inputs, or relative to `r`.
u32 Sum_graph::groups( const Form& f, const Form* r )
{
    std::array<Element, 64> g;
    u32                     size = 0;
    for( u32 j = 0; j < n_; ++j )
    {
        if( f[j] != field_->zero() )
        {
            g[size++] = field_->orbit( r ? field_->div( f[j], ( *r )[j] ) : f[j] ).first;
        }
    }
    std::sort( g.begin(), g.begin() + size );
    return static_cast<u32>( std::unique( g.begin(), g.begin() + size ) - g.begin() );
}

void Sum_graph::activate( u32 c )
{
    if( is_active_[c] )
    {
        return;
    }
    is_active_[c] = true;
    active_.push_back( c );
    for( const Graph_node& n : nodes_[c] )
    {
        if( n.kind == Op_kind::Scale )
        {
            activate( n.a );
        }
    }
}

void Sum_graph::add_node( u32 c, Graph_node n )
{
    if( n.kind == Op_kind::Add && n.a > n.b )
    {
        std::swap( n.a, n.b );
    }
    if( !node_set_.insert( { c, n.k, n.a, n.b, n.kind } ).second )
    {
        if( n.input )
        {
            for( Graph_node& m : nodes_[c] )
            {
                if( m.kind == n.kind && m.k == n.k && m.a == n.a && m.b == n.b )
                {
                    m.input = true;
                }
            }
        }
        return;
    }
    nodes_[c].push_back( n );
    ++steps_;
}

u32 Sum_graph::get( const Form& f )
{
    ++steps_;
    if( const auto found = ids_.find( f ); found != ids_.end() )
    {
        return found->second;
    }

    const u32 c = static_cast<u32>( forms_.size() );
    ids_.emplace( f, c );
    const u64 s = support( f );
    forms_.push_back( f );
    supp_.push_back( s );
    nodes_.emplace_back();
    is_active_.push_back( false );
    is_output_.push_back( false );

    const Element e = lead( f, s );
    if( e == field_->one() )
    {
        if( count( s ) == 1 )
        {
            nodes_[c].push_back( { Op_kind::Input, true, first( s ), 0, 0, {} } );
        }
        return c;
    }

    // Z/2^w: a non-unit lead divides only a single input's form.
    if( !field_->invertible( e ) )
    {
        if( count( s ) == 1 )
        {
            Form x        = f;
            x[first( s )] = field_->one();
            const u32 t   = get( x );
            nodes_[c].push_back( { Op_kind::Scale, true, e.v, t, 0, {} } );
        }
        return c;
    }

    const u32 t = get( scaled( f, field_->inv( e ) ) );
    nodes_[c].push_back( { Op_kind::Scale, count( s ) == 1, e.v, t, 0, {} } );
    // a (b x) = (ab) x: scale from every other non-unit multiple of the same normalised form.
    for( const auto [d, ed] : family_[t] )
    {
        const Element k = field_->div( e, ed );
        if( field_->is_unit( k ) )
        {
            continue;
        }
        add_node( c, { Op_kind::Scale, false, k.v, d, 0, {} } );
        add_node( d, { Op_kind::Scale, false, field_->inv( k ).v, c, 0, {} } );
    }
    family_[t].emplace_back( c, e );
    return c;
}

void Sum_graph::split( u32 c, u64 mask, Why why )
{
    const Form f = forms_[c];
    const u32  a = get( restricted( f, mask ) );
    const u32  b = get( restricted( f, ~mask ) );
    add_node( c, { Op_kind::Add, why == Why::Input, 0, a, b, {} } );
    if( why == Why::Shared )
    {
        activate( a );
    }
}

// Outputs R with R|I_g proportional to p|I_g on every ratio group I_g of the pair: the pieces p is
// built from.
std::vector<u32> Sum_graph::references( u32 p, u32 q, u64 both )
{
    std::vector<u32> refs;
    for( const u32 r : outputs_ )
    {
        if( r == p || r == q || ( supp_[r] & both ) != both )
        {
            continue;
        }

        std::vector<std::pair<Element, Element>> seen; // ratio q/p -> ratio r/p on that group
        bool                                     ok = true;
        for( u64 m = both; m && ok; m &= m - 1 )
        {
            const u32     j  = first( m );
            const Element ip = field_->inv( forms_[p][j] );
            const Element qp = field_->mul( forms_[q][j], ip );
            const Element rp = field_->mul( forms_[r][j], ip );
            const auto    at = std::ranges::find( seen, qp, &std::pair<Element, Element>::first );
            if( at == seen.end() )
            {
                seen.emplace_back( qp, rp );
            }
            else
            {
                ok = at->second == rp;
            }
        }
        // Its pieces must exist: each multi-input group a class, each single input unscaled.
        for( u64 m = both; m && ok; )
        {
            const u32     j = first( m );
            const Element k = field_->div( forms_[q][j], forms_[p][j] );
            u64           g = 0;
            for( u64 t = both; t; t &= t - 1 )
            {
                const u32 i = first( t );
                if( field_->div( forms_[q][i], forms_[p][i] ) == k )
                {
                    g |= 1ull << i;
                }
            }
            m &= ~g;
            if( count( g ) == 1 )
            {
                ok = field_->is_unit( forms_[r][j] );
            }
            else
            {
                const Form f = restricted( forms_[r], g );
                ok           = ids_.contains( scaled( f, field_->inv( lead( f, g ) ) ) );
            }
        }
        if( ok )
        {
            refs.push_back( r );
        }
    }
    return refs;
}

// p|I = A + B with A = (p + lq)/2, B = (p - lq)/2 on I; then lq|I = A - B. `least` holds, per
// reference (inputs first), min( groups( P ), groups( Q ) ), ~0 until known.
void Sum_graph::butterfly( u32 p, u32 q, u64 both, Element l, const std::vector<u32>& refs, std::vector<u32>& least )
{
    const Form P = restricted( forms_[p], both );
    const Form Q = restricted( forms_[q], both );
    Form       A = P, B = P, nB = P, lQ = P;
    for( u64 m = both; m; m &= m - 1 )
    {
        const u32     j  = first( m );
        const Element lq = field_->mul( l, Q[j] );
        lQ[j]            = lq;
        A[j]             = field_->mul( half_, field_->add( P[j], lq ) );
        B[j]             = field_->mul( half_, field_->add( P[j], field_->neg( lq ) ) );
        nB[j]            = field_->neg( B[j] );
    }
    if( support( A ) == 0 || support( B ) == 0 )
    {
        return;
    }

    // Kept only if the halves together need no more distinct constants than p or q, counted over the
    // inputs or over the pieces of a reference output.
    const auto pq = [&]( size_t i, const Form* R )
    {
        if( least[i] == ~u32 { 0 } )
        {
            least[i] = std::min( groups( P, R ), groups( Q, R ) );
        }
        return least[i];
    };
    bool keep = groups( A ) + groups( B ) <= pq( 0, nullptr );
    if( field_->is_unit( l ) )
    {
        for( size_t i = 0; i < refs.size() && !keep; ++i )
        {
            const Form* R = &forms_[refs[i]];
            keep          = groups( A, R ) + groups( B, R ) <= pq( i + 1, R );
        }
    }
    if( !keep )
    {
        return;
    }

    if( both != supp_[p] )
    {
        split( p, both, Why::Shared );
    }
    if( both != supp_[q] )
    {
        split( q, both, Why::Shared );
    }
    const u32 a  = get( A );
    const u32 b  = get( B );
    const u32 nb = get( nB );
    add_node( get( P ), { Op_kind::Add, false, 0, a, b, {} } );
    add_node( get( lQ ), { Op_kind::Add, false, 0, a, nb, {} } );
    activate( a );
    activate( b );
}

// A class with no active part is split by the largest active classes it contains up to a scalar.
void Sum_graph::contain()
{
    for( u32 c = 0; c < forms_.size(); ++c )
    {
        if( is_active_[c] || count( supp_[c] ) < 3 )
        {
            continue;
        }

        std::vector<std::pair<i32, u64>> inside;
        for( const u32 q : active_ )
        {
            const u64 sq = supp_[q];
            if( ( sq & supp_[c] ) != sq || sq == supp_[c] || count( sq ) < 2 )
            {
                continue;
            }
            std::optional<Element> d;
            bool                   same = true;
            for( u64 m = sq; m && same; m &= m - 1 )
            {
                const u32 j = first( m );
                if( !field_->invertible( forms_[q][j] ) )
                {
                    same = false;
                    break;
                }
                const Element e = field_->div( forms_[c][j], forms_[q][j] );
                same            = !d || *d == e;
                d               = e;
            }
            if( same )
            {
                inside.emplace_back( -static_cast<i32>( count( sq ) ), sq );
            }
        }
        std::ranges::sort( inside );
        for( size_t i = 0; i < inside.size() && i < k_contained; ++i )
        {
            split( c, inside[i].second, Why::Contained );
        }
    }
}

// Complex: +-1, +-i free, real or imaginary 0+2, |Re| = |Im| or a part +-1 2+2, else 2+4. Real: 0+1.
Cost Sum_graph::scale_cost( Element a )
{
    const Element s = field_->mul( a, a );
    if( field_->mul( s, s ) == field_->one() )
    {
        return {};
    }
    if( real_ )
    {
        return { 0, 1 };
    }
    const Element c = field_->conjugate( a );
    if( c == a || c == field_->neg( a ) )
    {
        return { 0, 2 };
    }
    const Element re = field_->add( a, c );                // 2 Re
    const Element im = field_->add( a, field_->neg( c ) ); // 2i Im
    if( field_->conjugate( s ) == field_->neg( s ) || re == field_->rational( 2 ) || re == field_->rational( -2 ) ||
        field_->mul( im, im ) == field_->rational( -4 ) )
    {
        return { 2, 2 };
    }
    return { 2, 4 };
}

// Orbits under the free units, with rep = u * form where u makes the lead canonical; then costs.
void Sum_graph::finish()
{
    std::unordered_map<Form, u32, Form_hash> orbit_ids( 0, Form_hash { n_ } );
    for( u32 c = 0; c < forms_.size(); ++c )
    {
        const auto [rep, u]        = field_->orbit( lead( forms_[c], supp_[c] ) );
        const auto [found, is_new] = orbit_ids.try_emplace( scaled( forms_[c], u ), static_cast<u32>( orbit_ids.size() ) );
        orbit_.push_back( found->second );
        unit_.push_back( field_->inv( u ) );
    }
    for( std::vector<Graph_node>& ns : nodes_ )
    {
        for( Graph_node& n : ns )
        {
            if( n.kind == Op_kind::Add )
            {
                n.cost = { real_ ? 1u : 2u, 0 };
            }
            else if( n.kind == Op_kind::Scale )
            {
                n.cost = scale_cost( Element { n.k } );
            }
        }
    }
}

Sum_graph::Sum_graph( Field& field, const std::vector<Row>& rows, u64 fuel )
    : field_( &field ),
      n_( static_cast<u32>( rows.at( 0 ).size() ) ),
      real_( field.kind() == Ring_kind::Real || ( field.kind() == Ring_kind::Integer && field.n() <= 2 ) ),
      halves_( field.kind() != Ring_kind::Integer ),
      fuel_( fuel ),
      base_( field.spent() ),
      ids_( 0, Form_hash { n_ } )
{
    assert( n_ <= 64 && rows.size() <= 64 );
    if( halves_ )
    {
        half_ = field.rational( 1, 2 );
    }
    if( real_ )
    {
        roots_of_unity_ = { field.one(), field.minus_one() };
    }
    else
    {
        for( u32 k = 0; k < field.n(); ++k )
        {
            roots_of_unity_.push_back( field.root( k ) );
        }
        for( u32 k = 0; k < field.n() && field.n() % 2 != 0; ++k )
        {
            roots_of_unity_.push_back( field.neg( field.root( k ) ) );
        }
    }

    // Each row, and its input chain: the input program, marked.
    for( const Row& row : rows )
    {
        Form f;
        f.fill( field.zero() );
        std::ranges::copy( row, f.begin() );
        assert( support( f ) != 0 );
        const u32 r = get( f );
        roots_.push_back( r );
        outputs_.push_back( r );
        is_output_[r] = true;
        activate( r );

        Form p;
        p.fill( field.zero() );
        std::optional<u32> prev;
        for( u32 j = 0; j < n_; ++j )
        {
            if( f[j] == field.zero() )
            {
                continue;
            }
            p[j]        = f[j];
            const u32 c = get( p );
            if( prev )
            {
                split( c, supp_[*prev], Why::Input );
            }
            prev = c;
        }
    }

    // Rounds over the newly active classes. Out of fuel, the round's finds so far are kept and every
    // class gets its fallback node.
    struct Pair
    {
        u32     p, q;
        Element l;
        u32     refs;
    };
    struct Found
    {
        u32 c;
        u64 part, group;
    };
    size_t done = 0;
    while( done < active_.size() )
    {
        const size_t                  end = active_.size();
        std::vector<Found>            found;
        std::vector<Pair>             pairs;
        std::vector<std::vector<u32>> refs;
        for( size_t pi = done; pi < end && !burnt(); ++pi )
        {
            const u32 p = active_[pi];
            if( count( supp_[p] ) < 2 )
            {
                continue;
            }
            for( size_t qi = 0; qi < end; ++qi )
            {
                if( qi == pi || ( qi >= done && qi < pi ) )
                {
                    continue;
                }
                const u32 q    = active_[qi];
                const u64 both = supp_[p] & supp_[q];
                ++steps_;
                if( count( both ) < 2 )
                {
                    continue;
                }

                std::vector<std::pair<Element, u64>> by_ratio;
                for( u64 m = both; m; m &= m - 1 )
                {
                    const u32 j = first( m );
                    if( !field.invertible( forms_[q][j] ) )
                    {
                        continue;
                    }
                    const Element r  = field.div( forms_[p][j], forms_[q][j] );
                    const auto    at = std::ranges::find( by_ratio, r, &std::pair<Element, u64>::first );
                    if( at == by_ratio.end() )
                    {
                        by_ratio.emplace_back( r, 1ull << j );
                    }
                    else
                    {
                        at->second |= 1ull << j;
                    }
                }
                // Several groups from one pair are chained, so each remainder is built from the rest.
                std::vector<std::pair<i32, u64>> gs;
                for( const auto [r, I] : by_ratio )
                {
                    if( count( I ) >= 2 )
                    {
                        gs.emplace_back( -static_cast<i32>( count( I ) ), I );
                    }
                }
                std::ranges::sort( gs );
                for( const u32 c : { p, q } )
                {
                    u64 part = supp_[c];
                    for( const auto [_, I] : gs )
                    {
                        if( I == part )
                        {
                            break;
                        }
                        found.push_back( { c, part, I } );
                        part &= ~I;
                    }
                }
                if( halves_ && by_ratio.size() > 1 )
                {
                    refs.emplace_back();
                    for( const Element l : roots_of_unity_ )
                    {
                        pairs.push_back( { p, q, l, static_cast<u32>( refs.size() - 1 ) } );
                    }
                }
            }
        }

        for( const Found& f : found )
        {
            if( !burnt() )
            {
                split( f.part == supp_[f.c] ? f.c : get( restricted( forms_[f.c], f.part ) ), f.group, Why::Shared );
            }
        }
        // References after this round's shared parts exist, so their pieces can be found.
        for( const Pair& b : pairs )
        {
            if( b.l == roots_of_unity_[0] && is_output_[b.p] && is_output_[b.q] )
            {
                refs[b.refs] = references( b.p, b.q, supp_[b.p] & supp_[b.q] );
            }
        }
        std::vector<u32> least;
        for( size_t i = 0; i < pairs.size(); ++i )
        {
            const Pair& b = pairs[i];
            if( i == 0 || b.refs != pairs[i - 1].refs )
            {
                least.assign( refs[b.refs].size() + 1, ~u32 { 0 } );
            }
            if( !burnt() )
            {
                butterfly( b.p, b.q, supp_[b.p] & supp_[b.q], b.l, refs[b.refs], least );
            }
        }

        done = out_of_fuel_ ? active_.size() : end;
        if( done == active_.size() )
        {
            if( !out_of_fuel_ )
            {
                contain();
            }
            for( u32 c = 0; c < forms_.size(); ++c )
            {
                if( nodes_[c].empty() )
                {
                    split( c, supp_[c] & -supp_[c], Why::Fallback );
                }
            }
        }
    }
    finish();
}
} // namespace keel
