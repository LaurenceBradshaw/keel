// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/group_order.h"
#include "ir/call_graph.h"

namespace keel
{
Group_order::Group_order( const std::vector<Function>& functions, const std::vector<std::vector<u32>>& also, Optimise optimise )
    : functions_( functions ),
      also_( also ),
      optimise_( std::move( optimise ) )
{
    add( 0 );
}

void Group_order::add( u32 first )
{
    std::vector<std::vector<u32>> edges = call_edges( functions_ );
    edges_.resize( functions_.size() );
    group_of_.resize( functions_.size() );
    for( u32 f = first; f < functions_.size(); ++f )
    {
        edges_[f] = std::move( edges[f] );
        if( f < also_.size() )
        {
            edges_[f].insert( edges_[f].end(), also_[f].begin(), also_[f].end() );
        }
    }
    for( std::vector<u32>& group : callees_first( functions_, also_ ) )
    {
        if( group.front() < first )
        {
            continue;
        }
        for( const u32 f : group )
        {
            group_of_[f] = static_cast<u32>( groups_.size() );
        }
        groups_.push_back( std::move( group ) );
        state_.push_back( State::Waiting );
    }
}

bool Group_order::want( u32 f )
{
    want_group( group_of_[f] );
    return state_[group_of_[f]] == State::Done;
}

void Group_order::want_all()
{
    for( u32 g = 0; g < groups_.size(); ++g )
    {
        want_group( g );
    }
}

void Group_order::want_group( u32 g )
{
    if( state_[g] != State::Waiting )
    {
        return;
    }
    state_[g] = State::Optimising;
    // Copies throughout: optimising may add groups.
    const std::vector<u32> group = groups_[g];
    for( const u32 f : group )
    {
        for( const u32 c : std::vector<u32>( edges_[f] ) )
        {
            want_group( group_of_[c] );
        }
    }
    optimise_( group );
    state_[g] = State::Done;
}
} // namespace keel
