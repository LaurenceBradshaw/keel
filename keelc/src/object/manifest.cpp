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
    out += manifest.debug ? fmt::format( "mode debug\nlevel {}\ncpu {}\n", manifest.level, manifest.cpu ) : "mode release\n";
    out += manifest.overflow ? "overflow\n" : "";
    out += manifest.llvm ? "backend llvm\n" : "backend c\n";
    out += fmt::format( "target {}\n", platform_name( manifest.platform ) );
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
    bool     has_format = false, has_build = false, has_mode = false, has_level = false, has_cpu = false, has_backend = false,
         has_target = false, has_module = false, has_source = false;

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
        else if( key == "level" && rest.size() == 1 && rest[0] >= '0' && rest[0] <= '3' )
        {
            manifest.level = u32( rest[0] - '0' );
            has_level      = true;
        }
        else if( key == "cpu" && !rest.empty() )
        {
            manifest.cpu = rest;
            has_cpu      = true;
        }
        else if( key == "overflow" && rest.empty() )
        {
            manifest.overflow = true;
        }
        else if( key == "backend" && ( rest == "c" || rest == "llvm" ) )
        {
            manifest.llvm = rest == "llvm";
            has_backend   = true;
        }
        else if( key == "target" && ( rest == platform_name( Platform::Linux ) || rest == platform_name( Platform::Windows ) ) )
        {
            manifest.platform = rest == platform_name( Platform::Windows ) ? Platform::Windows : Platform::Linux;
            has_target        = true;
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

    if( !has_build || !has_mode || has_level != manifest.debug || has_cpu != manifest.debug || !has_backend || !has_target ||
        !has_module || !has_source )
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
        .level       = 1,
        .cpu         = "x86-64-v3",
        .overflow    = true,
        .llvm        = true,
        .platform    = Platform::Windows,
        .module      = "kl::list",
        .source      = "/src/my kl/list.kl",
        .source_hash = 0x0123456789abcdef,
        .depends     = { { "kl::optional", 42 } },
    };

    const std::string text = write_manifest( written );
    REQUIRE(
        text == "keel object 2\nbuild abc123\nmode debug\nlevel 1\ncpu x86-64-v3\noverflow\nbackend llvm\ntarget "
                "x86_64-windows\nmodule "
                "kl::list\nsource /src/my kl/list.kl "
                "0123456789abcdef\ndepends kl::optional 000000000000002a\n"
    );

    const std::optional<Manifest> read = read_manifest( text );
    REQUIRE( read.has_value() );
    REQUIRE( read->format == 2 );
    REQUIRE( read->build == written.build );
    REQUIRE( read->debug );
    REQUIRE( read->level == 1 );
    REQUIRE( read->cpu == written.cpu );
    REQUIRE( read->overflow );
    REQUIRE( read->llvm );
    REQUIRE( read->platform == Platform::Windows );
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
    const std::string_view head = "keel object 2\nbuild b\nmode debug\nlevel 0\ncpu x86-64\n";
    REQUIRE( read_manifest( std::string( head ) + "backend c\ntarget x86_64-linux\nmodule m\nsource m.kl 0\n" ).has_value() );
    REQUIRE( !read_manifest( "keel object 2\nbuild b\nmode fast\nbackend c\ntarget x86_64-linux\nmodule m\nsource m.kl 0\n" )
                  .has_value() );
    const std::string_view tail = "backend c\ntarget x86_64-linux\nmodule m\nsource m.kl 0\n";
    REQUIRE( !read_manifest( "keel object 2\nbuild b\nmode debug\n" + std::string( tail ) ).has_value() ); // no level
    REQUIRE( !read_manifest( "keel object 2\nbuild b\nmode release\nlevel 2\n" + std::string( tail ) ).has_value() );
    REQUIRE( !read_manifest( "keel object 2\nbuild b\nmode debug\nlevel 4\ncpu x86-64\n" + std::string( tail ) ).has_value() );
    REQUIRE( !read_manifest( "keel object 2\nbuild b\nmode debug\nlevel 1\n" + std::string( tail ) ).has_value() ); // no cpu
    REQUIRE( !read_manifest( "keel object 2\nbuild b\nmode release\ncpu x86-64\n" + std::string( tail ) ).has_value() );
    REQUIRE( !read_manifest( std::string( head ) + "target x86_64-linux\nmodule m\nsource m.kl 0\n" ).has_value()
    );                                                                                                     // no backend
    REQUIRE( !read_manifest( std::string( head ) + "backend c\nmodule m\nsource m.kl 0\n" ).has_value() ); // no target
    REQUIRE( !read_manifest( std::string( head ) + "backend c\ntarget arm64-linux\nmodule m\nsource m.kl 0\n" ).has_value() );
    REQUIRE( !read_manifest( std::string( head ) + "backend c\ntarget x86_64-linux\nmodule m\nsource m.kl zz\n" ).has_value() );
    REQUIRE( !read_manifest( std::string( head ) + "backend c\ntarget x86_64-linux\nsource m.kl 0\n" ).has_value()
    ); // no module
}

} // namespace keel
#endif
