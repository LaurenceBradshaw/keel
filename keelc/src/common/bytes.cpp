// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/bytes.h"
#include <fmt/format.h>
#include <bit>
#include <limits>

namespace keel
{
void Byte_writer::put_u8( u8 value )
{
    out_.push_back( static_cast<char>( value ) );
}

void Byte_writer::put_u64( u64 value )
{
    while( value >= 128 )
    {
        put_u8( static_cast<u8>( value & 0x7f ) | 0x80 );
        value >>= 7;
    }
    put_u8( static_cast<u8>( value ) );
}

void Byte_writer::put_u32( u32 value )
{
    put_u64( value );
}

void Byte_writer::put_f64( f64 value )
{
    const u64 bits = std::bit_cast<u64>( value );
    for( u32 i = 0; i < 8; ++i )
    {
        put_u8( static_cast<u8>( bits >> ( i * 8 ) ) );
    }
}

void Byte_writer::put_string( std::string_view value )
{
    put_u32( narrow_cast<u32>( value.size() ) );
    out_ += value;
}

std::string Byte_writer::take()
{
    return std::move( out_ );
}

u8 Byte_reader::get_u8()
{
    if( at_ >= in_.size() )
    {
        fail();
        return 0;
    }

    return static_cast<u8>( in_[at_++] );
}

u64 Byte_reader::get_u64()
{
    u64 value = 0;
    for( u32 i = 0; i < 10; ++i )
    {
        const u8 byte = get_u8();
        if( failed() )
        {
            return 0;
        }

        // The tenth byte holds bit 63 alone.
        if( i == 9 && byte > 1 )
        {
            break;
        }

        value |= static_cast<u64>( byte & 0x7f ) << ( i * 7 );

        if( byte < 128 )
        {
            return value;
        }
    }

    fail();
    return 0;
}

u32 Byte_reader::get_u32()
{
    const u64 value = get_u64();
    if( failed() )
    {
        return 0;
    }

    if( value > std::numeric_limits<u32>::max() )
    {
        fail();
        return 0;
    }

    return static_cast<u32>( value );
}

f64 Byte_reader::get_f64()
{
    u64 bits = 0;
    for( u32 i = 0; i < 8; ++i )
    {
        const u8 byte = get_u8();
        if( failed() )
        {
            return 0;
        }

        bits |= ( static_cast<u64>( byte ) << ( i * 8 ) );
    }
    return std::bit_cast<f64>( bits );
}

std::string_view Byte_reader::get_string()
{
    const u32 size = get_u32();
    if( failed() )
    {
        return {};
    }

    if( size > in_.size() - at_ )
    {
        fail();
        return {};
    }

    const std::string_view result = in_.substr( at_, size );
    at_ += size;
    return result;
}

bool Byte_reader::failed() const
{
    return failed_;
}

void Byte_reader::fail()
{
    failed_ = true;
    at_     = in_.size();
}

u64 fnv1a( std::string_view bytes )
{
    constexpr u64 start = 0xcbf29ce484222325;
    constexpr u64 prime = 0x100000001b3;
    u64           hash  = start;
    for( const char byte : bytes )
    {
        hash ^= static_cast<u8>( byte );
        hash *= prime;
    }
    return hash;
}

std::string hex( u64 value )
{
    return fmt::format( "{:016x}", value );
}
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace keel
{

TEST_CASE( "bytes_round_trip", "[common][bytes]" )
{
    Byte_writer writer;

    SECTION( "integers at each edge of the varint" )
    {
        constexpr u64 max64 = std::numeric_limits<u64>::max();
        constexpr u32 max32 = std::numeric_limits<u32>::max();

        for( const u8 v : { u8( 0 ), u8( 127 ), u8( 128 ), u8( 255 ) } )
        {
            writer.put_u8( v );
        }
        for( const u32 v : { u32( 0 ), u32( 127 ), u32( 128 ), max32 } )
        {
            writer.put_u32( v );
        }
        for( const u64 v : { u64( 0 ), u64( 127 ), u64( 128 ), max64 } )
        {
            writer.put_u64( v );
        }

        const std::string bytes = writer.take();
        Byte_reader       reader( bytes );

        for( const u8 v : { u8( 0 ), u8( 127 ), u8( 128 ), u8( 255 ) } )
        {
            REQUIRE( reader.get_u8() == v );
        }
        for( const u32 v : { u32( 0 ), u32( 127 ), u32( 128 ), max32 } )
        {
            REQUIRE( reader.get_u32() == v );
        }
        for( const u64 v : { u64( 0 ), u64( 127 ), u64( 128 ), max64 } )
        {
            REQUIRE( reader.get_u64() == v );
        }
        REQUIRE_FALSE( reader.failed() );
    }

    // Compared bitwise, since a NaN is unequal to itself.
    SECTION( "floats keep every bit" )
    {
        const f64 values[] = { -0.0, std::numeric_limits<f64>::infinity(), std::nan( "7" ) };

        for( const f64 v : values )
        {
            writer.put_f64( v );
        }

        const std::string bytes = writer.take();
        Byte_reader       reader( bytes );

        for( const f64 v : values )
        {
            REQUIRE( std::bit_cast<u64>( reader.get_f64() ) == std::bit_cast<u64>( v ) );
        }
        REQUIRE_FALSE( reader.failed() );
    }

    SECTION( "strings keep every byte, a zero included" )
    {
        writer.put_string( "" );
        writer.put_string( std::string_view( "a\0b", 3 ) );

        const std::string bytes = writer.take();
        Byte_reader       reader( bytes );

        REQUIRE( reader.get_string().empty() );
        REQUIRE( reader.get_string() == std::string_view( "a\0b", 3 ) );
        REQUIRE_FALSE( reader.failed() );
    }
}

TEST_CASE( "bytes_reader_fails_on_bad_input", "[common][bytes]" )
{
    Byte_writer writer;
    std::string bytes;

    SECTION( "a truncated varint" )
    {
        writer.put_u64( 300 );
        bytes = writer.take();
        bytes.pop_back();

        Byte_reader reader( bytes );
        REQUIRE( reader.get_u64() == 0 );
        REQUIRE( reader.failed() );
    }

    SECTION( "a string longer than what is left" )
    {
        writer.put_u32( 5 );
        writer.put_u8( 'x' );
        bytes = writer.take();

        Byte_reader reader( bytes );
        REQUIRE( reader.get_string().empty() );
        REQUIRE( reader.failed() );
        REQUIRE( reader.get_u8() == 0 ); // the `x` is left, but a failed reader reads nothing
    }

    SECTION( "a varint of eleven bytes" )
    {
        bytes = std::string( 11, char( 0x80 ) );

        Byte_reader reader( bytes );
        REQUIRE( reader.get_u64() == 0 );
        REQUIRE( reader.failed() );
    }

    SECTION( "a u32 read of a wider value" )
    {
        writer.put_u64( u64( 1 ) << 32 );
        writer.put_u8( 1 );
        bytes = writer.take();

        Byte_reader reader( bytes );
        REQUIRE( reader.get_u32() == 0 );
        REQUIRE( reader.failed() );
        REQUIRE( reader.get_u8() == 0 );
    }
}

TEST_CASE( "fnv1a_matches_the_published_vectors", "[common][bytes]" )
{
    REQUIRE( hex( fnv1a( "" ) ) == "cbf29ce484222325" );
    REQUIRE( hex( fnv1a( "a" ) ) == "af63dc4c8601ec8c" );
    REQUIRE( hex( fnv1a( "foobar" ) ) == "85944171f73967e8" );

    // A char above 0x7f must not sign-extend.
    REQUIRE( hex( fnv1a( "\xff" ) ) == "af64724c8602eb6e" );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
