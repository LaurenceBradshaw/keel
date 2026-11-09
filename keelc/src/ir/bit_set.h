// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <cstddef>
#include <vector>
#include "common/types.h"

namespace keel
{

// Fixed-size bits in words, so a copy, compare or join is a word at a time even in a debug build.
class Bit_set
{
public:
    class Reference
    {
    public:
        Reference( u64& word, u64 mask )
            : word_( word ),
              mask_( mask )
        {
        }

        operator bool() const
        {
            return ( word_ & mask_ ) != 0;
        }

        Reference& operator=( bool value )
        {
            word_ = value ? word_ | mask_ : word_ & ~mask_;
            return *this;
        }

        Reference& operator=( const Reference& other )
        {
            return *this = static_cast<bool>( other );
        }

    private:
        u64& word_;
        u64  mask_;
    };

    Bit_set() = default;

    Bit_set( std::size_t size, bool value )
    {
        resize( size, value );
    }

    // Bits past the old size take `value`; the rest keep theirs.
    void resize( std::size_t size, bool value )
    {
        for( std::size_t i = size_; i < size && i % 64 != 0; ++i )
        {
            ( *this )[i] = value;
        }

        words_.resize( ( size + 63 ) / 64, value ? ~u64 { 0 } : 0 );
        size_ = size;
        trim();
    }

    std::size_t size() const
    {
        return size_;
    }

    bool operator[]( std::size_t i ) const
    {
        return ( words_[i / 64] >> ( i % 64 ) & 1 ) != 0;
    }

    Reference operator[]( std::size_t i )
    {
        return Reference( words_[i / 64], u64 { 1 } << ( i % 64 ) );
    }

    bool any() const
    {
        for( const u64 word : words_ )
        {
            if( word != 0 )
            {
                return true;
            }
        }
        return false;
    }

    // Adds `other`'s bits, of the same size; answers whether any was new.
    bool join( const Bit_set& other )
    {
        bool changed = false;
        for( std::size_t w = 0; w < words_.size(); ++w )
        {
            const u64 joined = words_[w] | other.words_[w];
            changed          = changed || joined != words_[w];
            words_[w]        = joined;
        }
        return changed;
    }

    bool operator==( const Bit_set& other ) const = default;

private:
    // Bits past the size stay clear, so equal sets compare equal.
    void trim()
    {
        if( size_ % 64 != 0 )
        {
            words_.back() &= ( u64 { 1 } << ( size_ % 64 ) ) - 1;
        }
    }

    std::vector<u64> words_;
    std::size_t      size_ = 0;
};

} // namespace keel
