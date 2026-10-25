// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/archive.h"
#include <string_view>
#include "common/types.h"

namespace keel
{

namespace
{

constexpr size_t k_header_size = 60;

// `text` left-aligned in `width` bytes, space-padded.
void field( std::string& out, std::string_view text, size_t width )
{
    out += text;
    out.append( width - text.size(), ' ' );
}

void header( std::string& out, std::string_view name, size_t size )
{
    field( out, name, 16 );
    field( out, "0", 12 );
    field( out, "0", 6 );
    field( out, "0", 6 );
    field( out, "644", 8 );
    field( out, std::to_string( size ), 10 );
    out += "`\n";
}

void big_endian( std::string& out, u32 value )
{
    out += char( value >> 24 );
    out += char( value >> 16 );
    out += char( value >> 8 );
    out += char( value );
}

size_t padded( size_t size )
{
    return size + size % 2;
}

void member( std::string& out, std::string_view name, std::string_view data )
{
    header( out, name, data.size() );
    out += data;

    if( data.size() % 2 != 0 )
    {
        out += '\n';
    }
}

} // namespace

std::string write_archive( std::span<const Archive_member> members )
{
    // Each member's header name: `name/`, or `/<offset>` into the long-name table.
    std::string              long_names;
    std::vector<std::string> names;

    for( const Archive_member& m : members )
    {
        if( m.name.size() <= 15 )
        {
            names.push_back( m.name + "/" );
        }
        else
        {
            names.push_back( "/" + std::to_string( long_names.size() ) );
            long_names += m.name + "/\n";
        }
    }

    size_t symbol_count = 0;
    size_t symbol_bytes = 0;

    for( const Archive_member& m : members )
    {
        for( const std::string& symbol : m.symbols )
        {
            ++symbol_count;
            symbol_bytes += symbol.size() + 1;
        }
    }

    const size_t index_size = 4 + 4 * symbol_count + symbol_bytes;

    // The index's offsets point past itself and the long-name table, so both are sized first.
    size_t offset = 8 + k_header_size + padded( index_size );
    if( !long_names.empty() )
    {
        offset += k_header_size + padded( long_names.size() );
    }

    std::string index;
    big_endian( index, u32( symbol_count ) );

    std::string symbol_names;

    for( const Archive_member& m : members )
    {
        for( const std::string& symbol : m.symbols )
        {
            big_endian( index, u32( offset ) );
            symbol_names += symbol;
            symbol_names += '\0';
        }

        offset += k_header_size + padded( m.data.size() );
    }

    index += symbol_names;

    std::string out = "!<arch>\n";
    member( out, "/", index );

    if( !long_names.empty() )
    {
        member( out, "//", long_names );
    }

    for( size_t i = 0; i < members.size(); ++i )
    {
        member( out, names[i], members[i].data );
    }

    return out;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{

namespace
{

u32 read_big_endian( const std::string& bytes, size_t at )
{
    return ( u32( u8( bytes[at] ) ) << 24 ) | ( u32( u8( bytes[at + 1] ) ) << 16 ) | ( u32( u8( bytes[at + 2] ) ) << 8 ) |
           u32( u8( bytes[at + 3] ) );
}

} // namespace

TEST_CASE( "write_archive_points_each_symbol_at_its_members_header", "[common][archive]" )
{
    const std::vector<Archive_member> members = {
        { "a.o", "first", { "kl_a", "kl_b" } },
        { "b.o", "second!", { "kl_c" } },
    };

    const std::string bytes = write_archive( members );

    REQUIRE( bytes.starts_with( "!<arch>\n" ) );
    REQUIRE( bytes.substr( 8, 16 ) == "/               " );

    const size_t index = 8 + k_header_size;
    REQUIRE( read_big_endian( bytes, index ) == 3 );

    const u32 first  = read_big_endian( bytes, index + 4 );
    const u32 second = read_big_endian( bytes, index + 8 );
    const u32 third  = read_big_endian( bytes, index + 12 );

    REQUIRE( first == second );
    REQUIRE( bytes.substr( first, 16 ) == "a.o/            " );
    REQUIRE( bytes.substr( first + 58, 2 ) == "`\n" );
    REQUIRE( bytes.substr( first + k_header_size, 5 ) == "first" );
    REQUIRE( bytes.substr( third, 16 ) == "b.o/            " );
    REQUIRE( bytes.substr( third + k_header_size, 7 ) == "second!" );

    REQUIRE( bytes.substr( index + 16, 15 ) == std::string( "kl_a\0kl_b\0kl_c\0", 15 ) );
}

TEST_CASE( "write_archive_pads_odd_data", "[common][archive]" )
{
    const std::vector<Archive_member> members = { { "a.o", "odd", {} } };

    const std::string bytes = write_archive( members );

    // The index of no symbols is four bytes, so the member's header follows it directly.
    const size_t at = 8 + k_header_size + 4;
    REQUIRE( bytes.substr( at, 16 ) == "a.o/            " );
    REQUIRE( bytes.substr( at + 48, 10 ) == "3         " );
    REQUIRE( bytes.substr( at + k_header_size ) == "odd\n" );
    REQUIRE( bytes.size() % 2 == 0 );
}

TEST_CASE( "write_archive_names_a_long_member_through_the_table", "[common][archive]" )
{
    const std::vector<Archive_member> members = {
        { "sixteen_bytes.o_", "x", { "kl_x" } },
        { "short.o", "y", {} },
    };

    const std::string bytes = write_archive( members );

    const size_t table = 8 + k_header_size + padded( 4 + 4 + 5 );
    REQUIRE( bytes.substr( table, 16 ) == "//              " );
    REQUIRE( bytes.substr( table + k_header_size, 18 ) == "sixteen_bytes.o_/\n" );

    const u32 member = read_big_endian( bytes, 8 + k_header_size + 4 );
    REQUIRE( member == table + k_header_size + 18 );
    REQUIRE( bytes.substr( member, 16 ) == "/0              " );
    REQUIRE( bytes.substr( member + k_header_size, 1 ) == "x" );
    REQUIRE( bytes.substr( member + k_header_size + 2, 16 ) == "short.o/        " );
}

TEST_CASE( "write_archive_writes_the_same_bytes_twice", "[common][archive]" )
{
    const std::vector<Archive_member> members = {
        { "a_rather_long_name.o", std::string( "\0\1\2", 3 ), { "kl_a" } },
        { "b.o", "b", { "kl_b" } },
    };

    REQUIRE( write_archive( members ) == write_archive( members ) );
}

} // namespace keel
#endif
