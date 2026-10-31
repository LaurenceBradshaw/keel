// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <cassert>
#include <initializer_list>
#include <vector>
#include "common/types.h"

namespace keel
{

// A vector indexed only by its own id type, so a `Block_id` cannot index the locals. `Id` is a
// struct holding a `u32 v`.
template <typename Id, typename T>
class Id_vector
{
public:
    // Every id in order, for a loop that also indexes a table of its own by `.v`.
    class Ids
    {
    public:
        class iterator
        {
        public:
            explicit iterator( u32 v )
                : v_( v )
            {
            }

            Id operator*() const
            {
                return Id { v_ };
            }

            iterator& operator++()
            {
                ++v_;
                return *this;
            }

            bool operator==( const iterator& other ) const = default;

        private:
            u32 v_;
        };

        explicit Ids( u32 count )
            : count_( count )
        {
        }

        iterator begin() const
        {
            return iterator( 0 );
        }

        iterator end() const
        {
            return iterator( count_ );
        }

    private:
        u32 count_;
    };

    Id_vector() = default;

    Id_vector( std::initializer_list<T> items )
        : items_( items )
    {
    }

    explicit Id_vector( std::vector<T> items )
        : items_( std::move( items ) )
    {
    }

    T& operator[]( Id id )
    {
        assert( id.v < items_.size() );
        return items_[id.v];
    }

    const T& operator[]( Id id ) const
    {
        assert( id.v < items_.size() );
        return items_[id.v];
    }

    Id push_back( T item )
    {
        items_.push_back( std::move( item ) );
        return Id { narrow_cast<u32>( items_.size() - 1 ) };
    }

    std::size_t size() const
    {
        return items_.size();
    }

    bool empty() const
    {
        return items_.empty();
    }

    Ids ids() const
    {
        return Ids( narrow_cast<u32>( items_.size() ) );
    }

    void reserve( std::size_t count )
    {
        items_.reserve( count );
    }

    void clear()
    {
        items_.clear();
    }

    T& front()
    {
        return items_.front();
    }

    const T& front() const
    {
        return items_.front();
    }

    T& back()
    {
        return items_.back();
    }

    const T& back() const
    {
        return items_.back();
    }

    auto begin()
    {
        return items_.begin();
    }

    auto end()
    {
        return items_.end();
    }

    auto begin() const
    {
        return items_.begin();
    }

    auto end() const
    {
        return items_.end();
    }

private:
    std::vector<T> items_;
};

} // namespace keel
