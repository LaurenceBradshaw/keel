// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <string>
#include <string_view>
#include "common/types.h"

namespace keel
{
class Byte_writer
{
public:
    void put_u8( u8 value );
    void put_u64( u64 value );
    void put_u32( u32 value );
    void put_f64( f64 value );
    void put_string( std::string_view value );

    std::string take();

private:
    std::string out_;
};

class Byte_reader
{
public:
    Byte_reader( std::string_view in )
        : in_( in )
    {
    }

    u8  get_u8();
    u64 get_u64();
    u32 get_u32();
    f64 get_f64();
    // A view into the input, so the input must outlive it.
    std::string_view get_string();

    bool failed() const;

private:
    void             fail();
    std::string_view in_;
    std::size_t      at_     = 0;
    bool             failed_ = false;
};

u64         fnv1a( std::string_view bytes );
std::string hex( u64 value );
} // namespace keel