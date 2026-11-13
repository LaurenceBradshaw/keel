// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "egraph/extract.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <map>
#include <queue>
#include <random>

namespace keel
{
namespace
{
constexpr u32 k_cyclic = 1u << 28; // the cost of a choice that reaches itself

u32 total( Cost c )
{
    return c.adds + c.muls;
}

// A node of an orbit: its children are orbits, `exact` the classes it was made from.
struct Orbit_node
{
    Cost             cost;
    std::vector<u32> kids;
    u32              cls;
    bool             input;
    Op_kind          kind;
    u32              k;
    std::vector<u32> exact;
};

f64 weight( const Orbit_node& n )
{
    return n.cost.adds + n.cost.muls * 1.001;
}

// The graph's classes grouped into orbits under the free units, and the search over choices of one
// node per orbit. A choice holds a node index per orbit, -1 for none.
class Extractor
{
public:
    Extractor( const Sum_graph& graph, const Extract_options& options );
    std::optional<Extraction> run( const std::vector<Row>& rows );

private:
    using Choice = std::vector<i32>;

    void              best_trees( const std::vector<bool>& free, const std::vector<f64>& w, f64 noise );
    void              mark( u32 o, const Choice& choice, std::vector<bool>& seen ) const;
    bool              acyclic( u32 o, const Choice& choice, std::vector<u8>& state ) const;
    Cost              dag_cost( const Choice& choice ) const;
    void              adopt( u32 o, const std::vector<bool>& frozen, Choice& choice, std::vector<bool>& seen ) const;
    Choice            forward_greedy( f64 noise );
    void              reextract( Choice& choice, u32 k, f64 noise );
    void              descent( Choice& choice, Cost& cost, f64 noise );
    bool              node_descent( Choice& choice, Cost& cost );
    bool              piece_descent( Choice& choice, Cost& cost );
    bool              broke() const;
    const Orbit_node& chosen( u32 o, const Choice& choice ) const
    {
        return orb_[o][static_cast<u32>( choice[o] )];
    }
    bool keep( const Choice& next, Choice& choice, Cost& cost ) const;
    f64  uniform( f64 hi );
    void shuffle( std::vector<u32>& v );

    const Sum_graph&                              graph_;
    Extract_options                               options_;
    std::vector<std::vector<Orbit_node>>          orb_;
    std::vector<std::vector<std::pair<u32, u32>>> parents_; // orbit -> (orbit, node) using it
    std::vector<u32>                              root_orbits_;
    std::vector<f64>                              amortised_;
    std::vector<f64>                              flat_;
    std::mt19937                                  rng_;
    u64                                           work_ = 0; // edges relaxed this restart

    // best_trees' results.
    std::vector<f64> best_;
    Choice           pick_;
};

Extractor::Extractor( const Sum_graph& graph, const Extract_options& options )
    : graph_( graph ),
      options_( options )
{
    u32 orbits = 0;
    for( u32 c = 0; c < graph.classes(); ++c )
    {
        orbits = std::max( orbits, graph.orbit( c ) + 1 );
    }
    orb_.resize( orbits );
    std::vector<std::map<std::vector<u32>, u32>> signatures( orbits );
    for( u32 c = 0; c < graph.classes(); ++c )
    {
        const u32 o = graph.orbit( c );
        for( const Graph_node& r : graph.nodes( c ) )
        {
            Orbit_node n { r.cost, {}, c, r.input, r.kind, r.k, {} };
            if( r.kind == Op_kind::Add )
            {
                n.exact = { r.a, r.b };
            }
            else if( r.kind == Op_kind::Scale )
            {
                n.exact = { r.a };
            }
            for( const u32 d : n.exact )
            {
                n.kids.push_back( graph.orbit( d ) );
            }
            // A node reaching its own orbit is no way to make it; one adding two of an orbit, x + ux,
            // is a scale the graph makes another way.
            if( std::ranges::find( n.kids, o ) != n.kids.end() || ( n.kids.size() == 2 && n.kids[0] == n.kids[1] ) )
            {
                continue;
            }

            std::vector<u32> sig = n.kids;
            std::ranges::sort( sig );
            sig.insert( sig.begin(), { u32( r.kind ), r.cost.adds, r.cost.muls, r.kind == Op_kind::Input ? r.k : 0 } );
            const auto [found, is_new] = signatures[o].try_emplace( sig, static_cast<u32>( orb_[o].size() ) );
            if( is_new )
            {
                orb_[o].push_back( std::move( n ) );
            }
            else
            {
                orb_[o][found->second].input = orb_[o][found->second].input || n.input;
            }
        }
    }

    parents_.resize( orbits );
    for( u32 o = 0; o < orbits; ++o )
    {
        for( u32 i = 0; i < orb_[o].size(); ++i )
        {
            for( const u32 d : orb_[o][i].kids )
            {
                parents_[d].emplace_back( o, i );
            }
        }
    }
    for( const u32 r : graph.roots() )
    {
        root_orbits_.push_back( graph.orbit( r ) );
    }
    assert( root_orbits_.size() <= 64 );

    // How many outputs could use each orbit: a cheap signal of how widely it can be shared.
    std::vector<u64> reach( orbits, 0 );
    for( u32 k = 0; k < root_orbits_.size(); ++k )
    {
        std::vector<u32> stack { root_orbits_[k] };
        while( !stack.empty() )
        {
            const u32 o = stack.back();
            stack.pop_back();
            if( reach[o] >> k & 1 )
            {
                continue;
            }
            reach[o] |= 1ull << k;
            for( const Orbit_node& n : orb_[o] )
            {
                stack.insert( stack.end(), n.kids.begin(), n.kids.end() );
            }
        }
    }
    for( u32 o = 0; o < orbits; ++o )
    {
        amortised_.push_back( 1.0 / std::max( 1, std::popcount( reach[o] ) ) );
    }
    flat_.assign( orbits, 1.0 );
}

f64 Extractor::uniform( f64 hi )
{
    return static_cast<f64>( rng_() ) * ( hi / 4294967296.0 );
}

// Fisher-Yates on the raw generator, so every standard library gives the same order.
void Extractor::shuffle( std::vector<u32>& v )
{
    for( size_t i = v.size(); i > 1; --i )
    {
        std::swap( v[i - 1], v[rng_() % i] );
    }
}

bool Extractor::broke() const
{
    return options_.fuel != 0 && work_ >= options_.fuel;
}

// Optimal tree costs where orbits in `free` cost nothing, node costs scaled by `w`. Knuth's
// generalisation of Dijkstra: a node is ready when all its child orbits are final.
void Extractor::best_trees( const std::vector<bool>& free, const std::vector<f64>& w, f64 noise )
{
    const size_t O = orb_.size();
    best_.assign( O, 1e300 );
    pick_.assign( O, -1 );
    std::vector<bool>             done( O, false );
    std::vector<std::vector<f64>> partial( O );
    std::vector<std::vector<u32>> left( O );
    using Item = std::pair<f64, u32>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    for( u32 o = 0; o < O; ++o )
    {
        partial[o].resize( orb_[o].size() );
        left[o].resize( orb_[o].size() );
        for( u32 i = 0; i < orb_[o].size(); ++i )
        {
            const Orbit_node& n = orb_[o][i];
            partial[o][i]       = weight( n ) * w[o] * ( 1 + ( noise > 0 ? uniform( noise ) : 0 ) );
            left[o][i]          = static_cast<u32>( n.kids.size() );
            if( n.kids.empty() && best_[o] > partial[o][i] )
            {
                best_[o] = partial[o][i];
                pick_[o] = static_cast<i32>( i );
                queue.emplace( best_[o], o );
            }
        }
        if( free[o] )
        {
            best_[o] = 0;
            queue.emplace( 0, o );
        }
    }
    while( !queue.empty() )
    {
        const auto [c, o] = queue.top();
        queue.pop();
        if( done[o] || c > best_[o] )
        {
            continue;
        }
        done[o] = true;
        work_ += parents_[o].size();
        for( const auto [p, i] : parents_[o] )
        {
            if( done[p] )
            {
                continue;
            }
            partial[p][i] += best_[o];
            if( --left[p][i] == 0 && partial[p][i] < best_[p] && !free[p] )
            {
                best_[p] = partial[p][i];
                pick_[p] = static_cast<i32>( i );
                queue.emplace( best_[p], p );
            }
        }
    }
}

void Extractor::mark( u32 o, const Choice& choice, std::vector<bool>& seen ) const
{
    if( seen[o] )
    {
        return;
    }
    seen[o] = true;
    for( const u32 d : chosen( o, choice ).kids )
    {
        mark( d, choice, seen );
    }
}

// 0 unseen, 1 on the path, 2 done.
bool Extractor::acyclic( u32 o, const Choice& choice, std::vector<u8>& state ) const
{
    if( state[o] )
    {
        return state[o] == 2;
    }
    if( choice[o] < 0 )
    {
        return false;
    }
    state[o] = 1;
    for( const u32 d : chosen( o, choice ).kids )
    {
        if( !acyclic( d, choice, state ) )
        {
            return false;
        }
    }
    state[o] = 2;
    return true;
}

Cost Extractor::dag_cost( const Choice& choice ) const
{
    std::vector<u8> state( orb_.size(), 0 );
    for( const u32 r : root_orbits_ )
    {
        if( !acyclic( r, choice, state ) )
        {
            return { k_cyclic, k_cyclic };
        }
    }
    std::vector<bool> seen( orb_.size(), false );
    for( const u32 r : root_orbits_ )
    {
        mark( r, choice, seen );
    }
    Cost c;
    for( u32 o = 0; o < orb_.size(); ++o )
    {
        if( seen[o] )
        {
            c.adds += chosen( o, choice ).cost.adds;
            c.muls += chosen( o, choice ).cost.muls;
        }
    }
    return c;
}

// Takes the tree picks for every orbit not frozen that `o` reaches.
void Extractor::adopt( u32 o, const std::vector<bool>& frozen, Choice& choice, std::vector<bool>& seen ) const
{
    if( seen[o] || frozen[o] )
    {
        return;
    }
    seen[o]   = true;
    choice[o] = pick_[o];
    if( choice[o] < 0 )
    {
        return;
    }
    for( const u32 d : chosen( o, choice ).kids )
    {
        adopt( d, frozen, choice, seen );
    }
}

// Forward greedy (Boyar-Peralta style, inside the graph): grows a base from the inputs, each step
// adding the ready orbit that most lowers the summed tree distance from the base to the outputs.
Extractor::Choice Extractor::forward_greedy( f64 noise )
{
    const size_t      O = orb_.size();
    std::vector<bool> in_base( O, false );
    Choice            choice( O, -1 );
    std::vector<u32>  tcount( O, 0 );
    for( const u32 r : root_orbits_ )
    {
        ++tcount[r];
    }
    std::vector<std::vector<u32>> left( O );
    std::vector<bool>             is_ready( O, false );
    std::vector<u32>              ready;
    for( u32 o = 0; o < O; ++o )
    {
        left[o].resize( orb_[o].size() );
        for( u32 i = 0; i < orb_[o].size(); ++i )
        {
            left[o][i] = static_cast<u32>( orb_[o][i].kids.size() );
            if( orb_[o][i].kind == Op_kind::Input )
            {
                in_base[o] = true;
                choice[o]  = static_cast<i32>( i );
            }
        }
    }
    best_trees( in_base, flat_, 0 );
    std::vector<f64> delta = best_;

    const auto enter = [&]( u32 o )
    {
        in_base[o] = true;
        for( const auto [p, i] : parents_[o] )
        {
            if( --left[p][i] == 0 && !in_base[p] && !is_ready[p] )
            {
                is_ready[p] = true;
                ready.push_back( p );
            }
        }
    };
    for( u32 o = 0; o < O; ++o )
    {
        if( in_base[o] )
        {
            in_base[o] = false;
            enter( o );
        }
    }
    u32 remaining = 0;
    for( const u32 r : root_orbits_ )
    {
        remaining += !in_base[r];
    }

    // The distances with `c` in the base, as changes to `delta`.
    std::vector<f64>  changed( O, 0 );
    std::vector<bool> is_changed( O, false );
    std::vector<u32>  touched;
    const auto        get       = [&]( u32 o ) { return is_changed[o] ? changed[o] : delta[o]; };
    const auto        propagate = [&]( u32 c )
    {
        for( const u32 o : touched )
        {
            is_changed[o] = false;
        }
        touched       = { c };
        changed[c]    = 0;
        is_changed[c] = true;
        using Item    = std::pair<f64, u32>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
        queue.emplace( 0, c );
        while( !queue.empty() )
        {
            const auto [d, o] = queue.top();
            queue.pop();
            if( d > get( o ) )
            {
                continue;
            }
            work_ += parents_[o].size();
            for( const auto [p, i] : parents_[o] )
            {
                if( in_base[p] )
                {
                    continue;
                }
                f64 v = weight( orb_[p][i] );
                for( const u32 k : orb_[p][i].kids )
                {
                    v += get( k );
                }
                if( v < get( p ) - 1e-9 )
                {
                    if( !is_changed[p] )
                    {
                        is_changed[p] = true;
                        touched.push_back( p );
                    }
                    changed[p] = v;
                    queue.emplace( v, p );
                }
            }
        }
        f64 gain = 0;
        for( const u32 o : touched )
        {
            if( tcount[o] && !in_base[o] )
            {
                gain += tcount[o] * ( delta[o] - changed[o] );
            }
        }
        return gain;
    };

    while( remaining > 0 )
    {
        if( broke() )
        {
            return {}; // the caller keeps its tree picks
        }
        i32 best      = -1;
        f64 best_gain = -1;
        for( const u32 c : ready )
        {
            if( in_base[c] )
            {
                continue;
            }
            const f64 g = propagate( c ) * ( 1 + ( noise > 0 ? uniform( noise ) : 0 ) );
            if( g > best_gain )
            {
                best_gain = g;
                best      = static_cast<i32>( c );
            }
        }
        if( best < 0 )
        {
            return {};
        }

        // The winner's cheapest node whose children are all in the base.
        const u32 b  = static_cast<u32>( best );
        i32       bi = -1;
        for( u32 i = 0; i < orb_[b].size(); ++i )
        {
            if( left[b][i] == 0 && ( bi < 0 || weight( orb_[b][i] ) < weight( orb_[b][static_cast<u32>( bi )] ) ) )
            {
                bi = static_cast<i32>( i );
            }
        }
        choice[b] = bi;
        propagate( b );
        for( const u32 o : touched )
        {
            delta[o] = changed[o];
        }
        remaining -= tcount[b];
        enter( b );
        std::erase_if( ready, [&]( u32 o ) { return in_base[o]; } );
    }
    return choice;
}

bool Extractor::keep( const Choice& next, Choice& choice, Cost& cost ) const
{
    const Cost c        = dag_cost( next );
    const bool improved = total( c ) < total( cost );
    if( total( c ) <= total( cost ) )
    {
        choice = next;
        cost   = c;
    }
    return improved;
}

// Re-extracts output k with every other output's orbits free.
void Extractor::reextract( Choice& choice, u32 k, f64 noise )
{
    std::vector<bool> frozen( orb_.size(), false );
    for( u32 j = 0; j < root_orbits_.size(); ++j )
    {
        if( j != k )
        {
            mark( root_orbits_[j], choice, frozen );
        }
    }
    best_trees( frozen, flat_, noise );
    std::vector<bool> seen( orb_.size(), false );
    adopt( root_orbits_[k], frozen, choice, seen );
}

void Extractor::descent( Choice& choice, Cost& cost, f64 noise )
{
    for( bool improved = true; improved; )
    {
        improved = false;
        std::vector<u32> order( root_orbits_.size() );
        for( u32 k = 0; k < order.size(); ++k )
        {
            order[k] = k;
        }
        shuffle( order );
        for( const u32 k : order )
        {
            if( broke() )
            {
                break;
            }
            Choice next = choice;
            reextract( next, k, noise * 0.1 );
            improved = keep( next, choice, cost ) || improved;
        }
    }
}

// Re-picks one used orbit's node, every other used orbit free.
bool Extractor::node_descent( Choice& choice, Cost& cost )
{
    bool              improved = false;
    std::vector<bool> used( orb_.size(), false );
    for( const u32 r : root_orbits_ )
    {
        mark( r, choice, used );
    }
    std::vector<u32> order;
    for( u32 o = 0; o < orb_.size(); ++o )
    {
        if( used[o] && orb_[o].size() > 1 )
        {
            order.push_back( o );
        }
    }
    shuffle( order );
    for( const u32 o : order )
    {
        if( broke() )
        {
            break;
        }
        std::vector<bool> frozen( orb_.size(), false );
        for( const u32 r : root_orbits_ )
        {
            mark( r, choice, frozen );
        }
        if( !frozen[o] )
        {
            continue;
        }
        frozen[o] = false;
        best_trees( frozen, flat_, 0 );
        Choice            next = choice;
        std::vector<bool> seen( orb_.size(), false );
        adopt( o, frozen, next, seen );
        improved = keep( next, choice, cost ) || improved;
    }
    return improved;
}

// A new shared piece X pays off only when all its users switch to it together. Re-picks every used
// orbit with a node using X at once, X free and everything only they used unfrozen; then pays for X.
bool Extractor::piece_descent( Choice& choice, Cost& cost )
{
    bool              improved = false;
    std::vector<bool> used( orb_.size(), false );
    for( const u32 r : root_orbits_ )
    {
        mark( r, choice, used );
    }
    std::vector<u32> order;
    for( u32 x = 0; x < orb_.size(); ++x )
    {
        if( used[x] )
        {
            continue;
        }
        u32 users = 0;
        i64 last  = -1;
        for( const auto [p, i] : parents_[x] )
        {
            if( used[p] && p != last )
            {
                ++users;
                last = p;
            }
        }
        if( users >= 2 )
        {
            order.push_back( x );
        }
    }
    shuffle( order );

    for( const u32 x : order )
    {
        if( broke() )
        {
            break;
        }
        std::vector<bool> now( orb_.size(), false ), stop( orb_.size(), false ), frozen( orb_.size(), false );
        for( const u32 r : root_orbits_ )
        {
            mark( r, choice, now );
        }
        if( now[x] )
        {
            continue;
        }
        std::vector<u32> users;
        for( const auto [p, i] : parents_[x] )
        {
            if( now[p] && !stop[p] )
            {
                stop[p] = true;
                users.push_back( p );
            }
        }
        if( users.size() < 2 )
        {
            continue;
        }

        const auto hold = [&]( auto& self, u32 o ) -> void
        {
            if( frozen[o] )
            {
                return;
            }
            frozen[o] = true;
            if( !stop[o] )
            {
                for( const u32 d : chosen( o, choice ).kids )
                {
                    self( self, d );
                }
            }
        };
        for( const u32 r : root_orbits_ )
        {
            hold( hold, r );
        }
        for( const u32 p : users )
        {
            frozen[p] = false;
        }
        frozen[x] = true;
        best_trees( frozen, flat_, 0 );
        Choice            next = choice;
        std::vector<bool> seen( orb_.size(), false );
        for( const u32 p : users )
        {
            adopt( p, frozen, next, seen );
        }

        std::vector<bool> held( orb_.size(), false );
        const auto        hold_next = [&]( auto& self, u32 o ) -> void
        {
            if( held[o] )
            {
                return;
            }
            held[o] = true;
            if( o != x && next[o] >= 0 )
            {
                for( const u32 d : chosen( o, next ).kids )
                {
                    self( self, d );
                }
            }
        };
        for( const u32 r : root_orbits_ )
        {
            hold_next( hold_next, r );
        }
        if( !held[x] )
        {
            continue; // no user took it
        }
        held[x] = false;
        best_trees( held, flat_, 0 );
        std::vector<bool> seen_x( orb_.size(), false );
        adopt( x, held, next, seen_x );
        improved = keep( next, choice, cost ) || improved;
    }
    return improved;
}

std::optional<Extraction> Extractor::run( const std::vector<Row>& rows )
{
    const size_t O = orb_.size();

    // The input program: every orbit's marked node, the rest by tree picks.
    Choice input_choice( O, -1 );
    for( u32 o = 0; o < O; ++o )
    {
        for( u32 i = 0; i < orb_[o].size(); ++i )
        {
            if( orb_[o][i].input )
            {
                input_choice[o] = static_cast<i32>( i );
                break;
            }
        }
    }
    const bool have_input = std::ranges::all_of( root_orbits_, [&]( u32 r ) { return input_choice[r] >= 0; } );
    Cost       input_cost { k_cyclic, k_cyclic };
    if( have_input )
    {
        best_trees( std::vector<bool>( O, false ), amortised_, 0 );
        for( u32 o = 0; o < O; ++o )
        {
            if( input_choice[o] < 0 )
            {
                input_choice[o] = pick_[o];
            }
        }
        input_cost = dag_cost( input_choice );
    }

    // Lowest total, ties to the lowest restart.
    Choice best_choice;
    Cost   best_cost { k_cyclic, k_cyclic };
    u64    spent = 0;
    for( u32 rs = 0; rs < options_.restarts; ++rs )
    {
        rng_.seed( options_.seed * 1000003u + rs );
        work_           = 0;
        const f64 noise = rs <= 1 ? 0 : 0.3;
        Choice    choice;
        if( rs == 0 && have_input )
        {
            choice = input_choice;
        }
        else
        {
            best_trees( std::vector<bool>( O, false ), amortised_, noise );
            choice = pick_;
            if( rs % 2 )
            {
                if( Choice g = forward_greedy( rs <= 2 ? 0 : 0.2 ); !g.empty() )
                {
                    choice = std::move( g );
                }
            }
        }
        Cost cost = dag_cost( choice );
        do
        {
            do
            {
                descent( choice, cost, noise );
            } while( node_descent( choice, cost ) );
        } while( piece_descent( choice, cost ) );
        spent += work_;
        if( total( cost ) < total( best_cost ) )
        {
            best_cost   = cost;
            best_choice = choice;
        }
    }
    if( total( best_cost ) >= k_cyclic )
    {
        return std::nullopt;
    }

    // The circuit: each used orbit's node once, each class its orbit's value times a unit.
    Field&  field   = graph_.field();
    Circuit circuit = field.kind() == Ring_kind::Integer ? Circuit( field ) : Circuit( field.kind() == Ring_kind::Real );
    std::vector<i64> made( O, -1 );
    const auto       scale = [&]( u32 a, Element k )
    { return field.kind() == Ring_kind::Integer ? circuit.scale( a, k ) : circuit.scale( a, field.value( k ) ); };
    const auto value = [&]( auto& self, u32 d ) -> u32
    {
        const u32         o = graph_.orbit( d );
        const Orbit_node& n = chosen( o, best_choice );
        if( made[o] < 0 )
        {
            u32 v = 0;
            switch( n.kind )
            {
            case Op_kind::Input:
                v = circuit.input( n.k );
                break;
            case Op_kind::Add:
                v = circuit.add( self( self, n.exact[0] ), self( self, n.exact[1] ) );
                break;
            case Op_kind::Scale:
                v = scale( self( self, n.exact[0] ), Element { n.k } );
                break;
            }
            made[o] = v;
        }
        return scale( static_cast<u32>( made[o] ), field.div( graph_.unit( d ), graph_.unit( n.cls ) ) );
    };
    for( size_t i = 0; i < rows.size(); ++i )
    {
        circuit.outputs.emplace( rows[i], value( value, graph_.roots()[i] ) );
    }
    return Extraction { std::move( circuit ), input_cost, best_cost, spent };
}
} // namespace

std::optional<Extraction> extract( const Sum_graph& graph, const std::vector<Row>& rows, const Extract_options& options )
{
    assert( rows.size() == graph.roots().size() );
    return Extractor( graph, options ).run( rows );
}
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <string>

namespace keel
{
namespace
{
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

std::vector<Row> parse( Field& field, const std::vector<std::string>& lines )
{
    std::vector<Row> rows;
    for( const std::string& line : lines )
    {
        std::istringstream s( line );
        Row                row;
        for( std::string t; s >> t; )
        {
            row.push_back( entry( field, t ) );
        }
        rows.push_back( row );
    }
    return rows;
}

std::vector<Row> dft( Field& field )
{
    std::vector<Row> rows( field.n() );
    for( u32 j = 0; j < field.n(); ++j )
    {
        for( u32 k = 0; k < field.n(); ++k )
        {
            rows[j].push_back( field.root( j * k ) );
        }
    }
    return rows;
}

// The circuit computes the map on random input.
bool computes( const Circuit& c, Field& field, u32 inputs )
{
    std::mt19937                   rng( 3 );
    std::vector<std::complex<f64>> x;
    for( u32 j = 0; j < inputs; ++j )
    {
        const f64 re = static_cast<f64>( rng() ) / 4294967296.0;
        const f64 im = static_cast<f64>( rng() ) / 4294967296.0;
        x.emplace_back( re, field.kind() == Ring_kind::Complex ? im : 0 );
    }
    const std::vector<std::complex<f64>> v = c.evaluate( x );
    for( const auto& [row, o] : c.outputs )
    {
        std::complex<f64> want = 0;
        for( u32 j = 0; j < inputs; ++j )
        {
            want += field.value( row[j] ) * x[j];
        }
        if( std::abs( want - v[o] ) > 1e-9 * ( 1 + std::abs( want ) ) )
        {
            return false;
        }
    }
    return true;
}

Extraction solve( Field& field, const std::vector<Row>& rows, u64 fuel = 0, u32 restarts = 16 )
{
    const Sum_graph graph( field, rows, fuel );
    auto            found = extract( graph, rows, { .restarts = restarts } );
    REQUIRE( found );
    CHECK( computes( found->circuit, field, static_cast<u32>( rows[0].size() ) ) );
    return std::move( *found );
}
} // namespace

// Recorded by the experiment's qgraph and descend at 16 restarts, found here in 2; dft7 (60/36)
// takes seconds.
TEST_CASE( "extract_finds_the_recorded_dfts", "[egraph][extract]" )
{
    for( const auto [n, cost] : { std::pair { 3u, Cost { 12, 4 } }, { 5u, { 32, 12 } }, { 6u, { 36, 8 } }, { 8u, { 52, 4 } } } )
    {
        Field field( Ring_kind::Complex, n );
        CAPTURE( n );
        CHECK( solve( field, dft( field ), 0, 2 ).best == cost );
    }
}

TEST_CASE( "extract_finds_the_recorded_real_maps", "[egraph][extract]" )
{
    Field      dct( Ring_kind::Real, 16 );
    const auto dct4 = parse(
        dct,
        {
            "1/2@0+1/2@0 1/2@0+1/2@0 1/2@0+1/2@0 1/2@0+1/2@0",
            "1/2@1+1/2@15 1/2@3+1/2@13 1/2@5+1/2@11 1/2@7+1/2@9",
            "1/2@2+1/2@14 1/2@6+1/2@10 1/2@10+1/2@6 1/2@14+1/2@2",
            "1/2@3+1/2@13 1/2@9+1/2@7 1/2@15+1/2@1 1/2@5+1/2@11",
        }
    );
    const Extraction d = solve( dct, dct4 );
    CHECK( d.input == Cost { 12, 12 } );
    CHECK( d.best == Cost { 8, 5 } );

    // The input program already wins.
    Field      lin( Ring_kind::Real, 4 );
    const auto lin4 = parse(
        lin,
        { "2/7@0 -1 -1 -1",
          "3/8@0 2/7@0 -1 -1",
          "4/11@0 3/8@0 2/7@0 -1",
          "5/16@0 4/11@0 3/8@0 2/7@0",
          "-1 5/16@0 4/11@0 3/8@0",
          "-1 -1 5/16@0 4/11@0",
          "-1 -1 -1 5/16@0" }
    );
    const Extraction l = solve( lin, lin4 );
    CHECK( l.input == Cost { 9, 16 } );
    CHECK( l.best == Cost { 9, 16 } );
}

// No 1/2 in Z/2^w, so no butterflies: the shared parts find n log n.
TEST_CASE( "extract_finds_an_integer_wht", "[egraph][extract]" )
{
    Field            ring( Ring_kind::Integer, 1, 32 );
    std::vector<Row> rows( 8 );
    for( u32 j = 0; j < 8; ++j )
    {
        for( u32 k = 0; k < 8; ++k )
        {
            rows[j].push_back( std::popcount( j & k ) % 2 ? ring.minus_one() : ring.one() );
        }
    }
    const Extraction e = solve( ring, rows );
    CHECK( e.input == Cost { 42, 0 } ); // rows share prefixes: 2 + 4 + 4 + 4 * 8
    CHECK( e.best == Cost { 24, 0 } );
}

TEST_CASE( "extract_gives_one_circuit_per_map", "[egraph][extract]" )
{
    Field a( Ring_kind::Complex, 8 );
    Field b( Ring_kind::Complex, 8 );
    CHECK( solve( a, dft( a ) ).circuit.text() == solve( b, dft( b ) ).circuit.text() );
}

TEST_CASE( "extract_out_of_fuel_keeps_the_input", "[egraph][extract]" )
{
    Field            field( Ring_kind::Complex, 8 );
    const Extraction e = solve( field, dft( field ), 1 );
    CHECK( e.best.adds + e.best.muls <= e.input.adds + e.input.muls );
    CHECK( e.input == Cost { 120, 8 } );
}
} // namespace keel
#endif
