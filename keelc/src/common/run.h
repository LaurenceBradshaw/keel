// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <cassert>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <vector>
#include "common/types.h"

namespace keel
{

// `count` items of a vector from `first`, read through the vector on every access, so a loop that
// appends to the vector while walking a run stays valid where a std::span would dangle. `T` is
// const for a run that may not be written.
template <typename T>
class Run
{
    using Item   = std::remove_const_t<T>;
    using Vector = std::conditional_t<std::is_const_v<T>, const std::vector<Item>, std::vector<Item>>;

public:
    class iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = Item;
        using difference_type   = std::ptrdiff_t;
        using pointer           = T*;
        using reference         = T&;

        iterator() = default;

        iterator( Vector* items, u32 at )
            : items_( items ),
              at_( at )
        {
        }

        T& operator*() const
        {
            return ( *items_ )[at_];
        }

        T* operator->() const
        {
            return &( *items_ )[at_];
        }

        iterator& operator++()
        {
            ++at_;
            return *this;
        }

        iterator operator++( int )
        {
            iterator before = *this;
            ++at_;
            return before;
        }

        bool operator==( const iterator& other ) const
        {
            return at_ == other.at_;
        }

    private:
        Vector* items_ = nullptr;
        u32     at_    = 0;
    };

    Run( Vector& items, u32 first, u32 count )
        : items_( &items ),
          first_( first ),
          count_( count )
    {
        assert( std::size_t { first } + count <= items.size() );
    }

    // A writable run reads as a read-only one.
    Run( const Run<Item>& other )
        requires std::is_const_v<T>
        : items_( other.items_ ),
          first_( other.first_ ),
          count_( other.count_ )
    {
    }

    T& operator[]( u32 i ) const
    {
        assert( i < count_ );
        return ( *items_ )[first_ + i];
    }

    u32 size() const
    {
        return count_;
    }

    bool empty() const
    {
        return count_ == 0;
    }

    // Where item `i` sits in the whole vector, for a table kept in step with it.
    u32 position( u32 i ) const
    {
        return first_ + i;
    }

    iterator begin() const
    {
        return iterator( items_, first_ );
    }

    iterator end() const
    {
        return iterator( items_, first_ + count_ );
    }

private:
    template <typename>
    friend class Run;

    Vector* items_;
    u32     first_;
    u32     count_;
};

} // namespace keel
