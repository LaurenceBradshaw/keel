// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <unordered_map>
#include <vector>
#include "ir/kir.h"

namespace keel
{
// Keys each held in a local: found by hash, dropped by a local an entry holds or mentions, or by
// being in memory. `Traits` gives `u64 hash( const Key& )`, `bool same( const Key&, const Key& )`,
// `void mentioned( const Key&, std::vector<Local_id>& )` and `bool in_memory( const Key& )`.
template <class Key, class Traits>
class Held_table
{
public:
    struct Entry
    {
        Key      key;
        Local_id local;
    };

    explicit Held_table( const Traits& traits )
        : traits_( &traits )
    {
    }

    const Entry* find( const Key& key ) const
    {
        auto [at, end] = by_hash_.equal_range( traits_->hash( key ) );
        for( ; at != end; ++at )
        {
            if( !gone_[at->second] && traits_->same( entries_[at->second].key, key ) )
            {
                return &entries_[at->second];
            }
        }
        return nullptr;
    }

    // `key` is not held.
    void add( Key key, Local_id local )
    {
        const u32 i = static_cast<u32>( entries_.size() );
        by_hash_.emplace( traits_->hash( key ), i );
        by_local_[local.v].push_back( i );
        std::vector<Local_id> mentioned;
        traits_->mentioned( key, mentioned );
        for( const Local_id l : mentioned )
        {
            if( l != local )
            {
                by_local_[l.v].push_back( i );
            }
        }
        if( traits_->in_memory( key ) )
        {
            in_memory_.push_back( i );
        }
        entries_.push_back( { std::move( key ), local } );
        gone_.push_back( false );
        ++live_;
    }

    // The entries held in `local` or mentioning it.
    void forget( Local_id local )
    {
        const auto found = by_local_.find( local.v );
        if( found == by_local_.end() )
        {
            return;
        }
        for( const u32 i : found->second )
        {
            drop( i );
        }
        by_local_.erase( found );
    }

    void forget_memory()
    {
        for( const u32 i : in_memory_ )
        {
            drop( i );
        }
        in_memory_.clear();
    }

    template <class Predicate>
    void erase_if( Predicate erased )
    {
        Held_table kept( *traits_ );
        for( u32 i = 0; i < entries_.size(); ++i )
        {
            if( !gone_[i] && !erased( entries_[i] ) )
            {
                kept.add( entries_[i].key, entries_[i].local );
            }
        }
        *this = std::move( kept );
    }

    void clear()
    {
        *this = Held_table( *traits_ );
    }

    std::size_t size() const
    {
        return live_;
    }

private:
    void drop( u32 i )
    {
        if( !gone_[i] )
        {
            gone_[i] = true;
            --live_;
        }
    }

    const Traits*                             traits_;
    std::vector<Entry>                        entries_;
    std::vector<bool>                         gone_;
    std::unordered_multimap<u64, u32>         by_hash_;
    std::unordered_map<u32, std::vector<u32>> by_local_;
    std::vector<u32>                          in_memory_;
    std::size_t                               live_ = 0;
};

inline u64 mixed( u64 h, u64 v )
{
    return ( h ^ v ) * 0x100000001b3ull;
}

// Locals a place is rooted at or indexes by, as `mentions` reads them.
inline void mentioned_by( const Function& func, const Place& place, std::vector<Local_id>& out )
{
    if( !place.is_global() && place.local.is_valid() )
    {
        out.push_back( place.local );
    }
    for( const Projection& p : func.projections_of( place ) )
    {
        if( p.kind == Projection_kind::Index )
        {
            out.push_back( p.index );
        }
    }
}

inline u64 place_hash( const Function& func, const Place& place )
{
    u64 h = mixed( mixed( 0xcbf29ce484222325ull, place.local.v ), place.global.v );
    for( const Projection& p : func.projections_of( place ) )
    {
        h = mixed( mixed( mixed( h, static_cast<u64>( p.kind ) ), p.index.v ), p.field.v );
    }
    return h;
}
} // namespace keel
