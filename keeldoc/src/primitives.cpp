// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "primitives.h"
#include <set>

namespace keeldoc
{
namespace
{

const std::set<std::string_view> k_primitives = {
    "bool",
    "f32",
    "f64",
    "i8",
    "i16",
    "i32",
    "i64",
    "never",
    "u8",
    "u16",
    "u32",
    "u64",
    "void",
};

// `text` without the blank lines around it.
std::string trim_lines( std::string_view text )
{
    const std::size_t first = text.find_first_not_of( '\n' );
    if( first == std::string_view::npos )
    {
        return {};
    }

    return std::string( text.substr( first, text.find_last_not_of( '\n' ) + 1 - first ) );
}

} // namespace

bool is_primitive( std::string_view word )
{
    return k_primitives.contains( word );
}

std::vector<Entry> parse_primitives( std::string_view source, std::string& intro )
{
    std::vector<Entry> entries;
    std::string        text;

    const auto flush = [&]
    {
        if( entries.empty() )
        {
            intro = trim_lines( text );
        }
        else
        {
            entries.back().group.overloads.front().doc = trim_lines( text );
        }
        text.clear();
    };

    while( !source.empty() )
    {
        const std::size_t      end  = source.find( '\n' );
        const std::string_view line = source.substr( 0, end );
        source.remove_prefix( end == std::string_view::npos ? source.size() : end + 1 );

        if( line.starts_with( "# " ) )
        {
            flush();
            const std::string name( line.substr( 2 ) );
            entries.push_back( { { name, { { "primitive", name, name, {} } } }, {} } );
            continue;
        }

        text += line;
        text += '\n';
    }

    flush();
    return entries;
}

} // namespace keeldoc

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keeldoc
{
namespace
{

TEST_CASE( "primitives_split_at_each_heading", "[primitives]" )
{
    std::string              intro;
    const std::vector<Entry> entries = parse_primitives( "Intro.\n\n# i8\n\nSmall.\n\nMore.\n# bool\nYes.\n", intro );

    REQUIRE( intro == "Intro." );
    REQUIRE( entries.size() == 2 );
    REQUIRE( entries[0].group.name == "i8" );
    REQUIRE( entries[0].group.overloads[0].declares == "primitive" );
    REQUIRE( entries[0].group.overloads[0].signature == "i8" );
    REQUIRE( entries[0].group.overloads[0].doc == "Small.\n\nMore." );
    REQUIRE( entries[1].group.overloads[0].doc == "Yes." );
}

// A primitive keelc gains must be documented, and one documented must still exist.
TEST_CASE( "primitives_document_each_primitive_once", "[primitives]" )
{
    std::string                intro;
    std::multiset<std::string> documented;
    for( const Entry& entry : parse_primitives( primitives_source(), intro ) )
    {
        documented.insert( entry.group.name );
    }

    REQUIRE( !intro.empty() );
    REQUIRE( documented == std::multiset<std::string>( k_primitives.begin(), k_primitives.end() ) );
}

} // namespace
} // namespace keeldoc
#endif // ENABLE_UNIT_TESTS
