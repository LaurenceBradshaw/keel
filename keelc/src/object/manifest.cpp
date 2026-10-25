// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "object/manifest.h"
#include <fmt/format.h>
#include <charconv>
#include "common/bytes.h"

namespace keel
{

namespace
{

std::optional<u64> parse_hex( std::string_view text )
{
    u64 value               = 0;
    const auto [end, error] = std::from_chars( text.data(), text.data() + text.size(), value, 16 );
    if( error != std::errc {} || end != text.data() + text.size() || text.empty() )
    {
        return std::nullopt;
    }
    return value;
}

// `line` split at its first space; the rest is empty when there is none.
std::pair<std::string_view, std::string_view> split( std::string_view line )
{
    const size_t space = line.find( ' ' );
    return space == std::string_view::npos ? std::pair { line, std::string_view {} }
                                           : std::pair { line.substr( 0, space ), line.substr( space + 1 ) };
}

} // namespace

std::string write_manifest( const Manifest& manifest )
{
    std::string out = fmt::format( "keel object {}\nbuild {}\n", manifest.format, manifest.build );
    out += manifest.debug ? "mode debug\n" : "mode release\n";
    out += manifest.overflow ? "overflow\n" : "";
    out += fmt::format( "module {}\nsource {} {}\n", manifest.module, manifest.source, hex( manifest.source_hash ) );

    for( const auto& [module, hash] : manifest.depends )
    {
        out += fmt::format( "depends {} {}\n", module, hex( hash ) );
    }
    return out;
}

std::optional<Manifest> read_manifest( std::string_view text )
{
    Manifest manifest;
    bool     has_format = false, has_build = false, has_mode = false, has_module = false, has_source = false;

    while( !text.empty() )
    {
        const size_t newline = text.find( '\n' );
        if( newline == std::string_view::npos )
        {
            return std::nullopt;
        }
        const std::string_view line = text.substr( 0, newline );
        text.remove_prefix( newline + 1 );

        const auto [key, rest] = split( line );

        if( !has_format )
        {
            const auto [object, number] = split( rest );
            const auto [end, error]     = std::from_chars( number.data(), number.data() + number.size(), manifest.format );
            if( key != "keel" || object != "object" || error != std::errc {} || end != number.data() + number.size() )
            {
                return std::nullopt;
            }
            has_format = true;
        }
        else if( key == "build" )
        {
            manifest.build = rest;
            has_build      = true;
        }
        else if( key == "mode" && ( rest == "debug" || rest == "release" ) )
        {
            manifest.debug = rest == "debug";
            has_mode       = true;
        }
        else if( key == "overflow" && rest.empty() )
        {
            manifest.overflow = true;
        }
        else if( key == "module" )
        {
            manifest.module = rest;
            has_module      = true;
        }
        else if( key == "source" || key == "depends" )
        {
            // The name may hold spaces, as a path can, so the hash is after the last.
            const size_t             space = rest.rfind( ' ' );
            const std::optional<u64> hash =
                space == std::string_view::npos ? std::nullopt : parse_hex( rest.substr( space + 1 ) );
            if( !hash )
            {
                return std::nullopt;
            }

            if( key == "source" )
            {
                manifest.source      = rest.substr( 0, space );
                manifest.source_hash = *hash;
                has_source           = true;
            }
            else
            {
                manifest.depends.emplace_back( std::string( rest.substr( 0, space ) ), *hash );
            }
        }
        else
        {
            return std::nullopt;
        }
    }

    if( !has_build || !has_mode || !has_module || !has_source )
    {
        return std::nullopt;
    }
    return manifest;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{

TEST_CASE( "manifest_round_trips", "[object][manifest]" )
{
    const Manifest written {
        .build       = "abc123",
        .debug       = true,
        .overflow    = true,
        .module      = "kl::list",
        .source      = "/src/my kl/list.kl",
        .source_hash = 0x0123456789abcdef,
        .depends     = { { "kl::optional", 42 } },
    };

    const std::string text = write_manifest( written );
    REQUIRE(
        text == "keel object 1\nbuild abc123\nmode debug\noverflow\nmodule kl::list\nsource /src/my kl/list.kl "
                "0123456789abcdef\ndepends kl::optional 000000000000002a\n"
    );

    const std::optional<Manifest> read = read_manifest( text );
    REQUIRE( read.has_value() );
    REQUIRE( read->format == 1 );
    REQUIRE( read->build == written.build );
    REQUIRE( read->debug );
    REQUIRE( read->overflow );
    REQUIRE( read->module == written.module );
    REQUIRE( read->source == written.source );
    REQUIRE( read->source_hash == written.source_hash );
    REQUIRE( read->depends == written.depends );
}

TEST_CASE( "manifest_refuses_what_it_did_not_write", "[object][manifest]" )
{
    const std::string good = write_manifest( { .build = "b", .module = "m", .source = "m.kl" } );
    REQUIRE( read_manifest( good ).has_value() );

    REQUIRE( !read_manifest( "" ).has_value() );
    REQUIRE( !read_manifest( "keel object\n" ).has_value() );
    REQUIRE( !read_manifest( good.substr( 0, good.size() - 1 ) ).has_value() ); // no final newline
    REQUIRE( !read_manifest( good + "colour blue\n" ).has_value() );
    REQUIRE( !read_manifest( "keel object 1\nbuild b\nmode fast\nmodule m\nsource m.kl 0\n" ).has_value() );
    REQUIRE( !read_manifest( "keel object 1\nbuild b\nmode debug\nmodule m\nsource m.kl zz\n" ).has_value() );
    REQUIRE( !read_manifest( "keel object 1\nbuild b\nmode debug\nsource m.kl 0\n" ).has_value() ); // no module
}

} // namespace keel
#endif
