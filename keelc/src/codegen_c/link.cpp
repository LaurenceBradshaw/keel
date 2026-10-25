// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_c/link.h"
#include <fmt/format.h>
#include <fstream>

namespace keel
{

Runtime find_runtime( const std::optional<std::string>& flag, const std::filesystem::path& folder )
{
    if( !flag.has_value() )
    {
        const std::filesystem::path path = folder / "libkeel_rt.a";
        std::ofstream               out( path, std::ios::binary );
        out.write( reinterpret_cast<const char*>( k_runtime ), static_cast<std::streamsize>( k_runtime_size ) );
        if( !out.flush() )
        {
            return Runtime { path, fmt::format( "could not write the runtime to '{}'", path.string() ) };
        }
        return Runtime { path, "" };
    }

    const std::filesystem::path path = *flag;
    std::error_code             ec;
    if( !std::filesystem::is_regular_file( path, ec ) )
    {
        return Runtime { path, fmt::format( "--runtime names '{}', which is not a file", path.string() ) };
    }
    return Runtime { path, "" };
}

namespace
{
// §7.7: C's UB becomes Keel's UB without these.
std::string compiler_and_flags( std::string_view compiler, std::string_view cflags )
{
    return fmt::format(
        "{} -fwrapv -fno-strict-aliasing -std=c11{}", compiler, cflags.empty() ? std::string() : fmt::format( " {}", cflags )
    );
}
} // namespace

std::string compile_command(
    std::string_view compiler, std::string_view cflags, const std::filesystem::path& source, const std::filesystem::path& object
)
{
    return fmt::format( "{} -c \"{}\" -o \"{}\"", compiler_and_flags( compiler, cflags ), source.string(), object.string() );
}

std::string link_command(
    std::string_view                       compiler,
    std::string_view                       cflags,
    std::span<const std::filesystem::path> objects,
    const std::filesystem::path&           runtime,
    const std::filesystem::path&           executable,
    std::span<const std::string>           linked
)
{
    std::string inputs;
    for( const std::filesystem::path& object : objects )
    {
        inputs += fmt::format( " \"{}\"", object.string() );
    }

    inputs += fmt::format( " \"{}\"", runtime.string() );

    for( const std::string& word : linked )
    {
        inputs += fmt::format( " \"{}\"", word );
    }

    return fmt::format( "{}{} -o \"{}\"", compiler_and_flags( compiler, cflags ), inputs, executable.string() );
}

std::string without_warning_flags( std::string_view cflags )
{
    std::string result;
    std::size_t start = 0;
    while( start < cflags.size() )
    {
        const std::size_t      end = cflags.find( ' ', start );
        const std::string_view word =
            end == std::string_view::npos ? cflags.substr( start ) : cflags.substr( start, end - start );

        if( !word.empty() && !word.starts_with( "-pedantic" ) &&
            ( !word.starts_with( "-W" ) || word.starts_with( "-Wl," ) || word.starts_with( "-Wa," ) ||
              word.starts_with( "-Wp," ) ) )
        {
            if( !result.empty() )
            {
                result += ' ';
            }
            result += word;
        }

        if( end == std::string_view::npos )
        {
            break;
        }
        start = end + 1;
    }

    return result;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include "common/temp_dir.h"

TEST_CASE( "find_runtime_prefers_the_flag", "[codegen_c][link]" )
{
    const keel::Temp_dir        dir;
    const std::filesystem::path named   = dir.write( "named.a", "" );
    const keel::Runtime         runtime = keel::find_runtime( named.string(), dir.path );

    REQUIRE( runtime.error.empty() );
    REQUIRE( runtime.path == named );
    REQUIRE( !std::filesystem::exists( dir.path / "libkeel_rt.a" ) );
}

TEST_CASE( "find_runtime_writes_the_runtime_it_carries", "[codegen_c][link]" )
{
    const keel::Temp_dir dir;
    const keel::Runtime  runtime = keel::find_runtime( std::nullopt, dir.path );

    REQUIRE( runtime.error.empty() );
    REQUIRE( runtime.path == dir.path / "libkeel_rt.a" );
    REQUIRE( std::filesystem::file_size( runtime.path ) == keel::k_runtime_size );

    std::ifstream in( runtime.path, std::ios::binary );
    std::string   magic( 8, '\0' );
    in.read( magic.data(), 8 );
    REQUIRE( magic == "!<arch>\n" );
}

TEST_CASE( "find_runtime_refuses_what_is_not_a_file", "[codegen_c][link]" )
{
    const keel::Temp_dir        dir;
    const std::filesystem::path absent = dir.path / "absent.a";

    SECTION( "a missing --runtime is named as the flag's" )
    {
        const keel::Runtime runtime = keel::find_runtime( absent.string(), dir.path );

        REQUIRE( runtime.error == "--runtime names '" + absent.string() + "', which is not a file" );
    }

    SECTION( "a directory is not a library" )
    {
        const keel::Runtime runtime = keel::find_runtime( dir.path.string(), dir.path );

        REQUIRE( runtime.error == "--runtime names '" + dir.path.string() + "', which is not a file" );
    }

    SECTION( "the carried runtime needs a folder to land in" )
    {
        const keel::Runtime runtime = keel::find_runtime( std::nullopt, absent );

        REQUIRE( runtime.error == "could not write the runtime to '" + ( absent / "libkeel_rt.a" ).string() + "'" );
    }
}

TEST_CASE( "compile_command_compiles_one_part_to_its_object", "[codegen_c][link]" )
{
    SECTION( "with flags" )
    {
        REQUIRE(
            keel::compile_command( "clang", "-O2 -g", "out/main.kl.c", "tmp/main.kl.o" ) ==
            "clang -fwrapv -fno-strict-aliasing -std=c11 -O2 -g -c \"out/main.kl.c\" -o \"tmp/main.kl.o\""
        );
    }

    SECTION( "without, leaving no gap" )
    {
        REQUIRE(
            keel::compile_command( "cc", "", "a b/main.kl.prelude.c", "main.kl.prelude.o" ) ==
            "cc -fwrapv -fno-strict-aliasing -std=c11 -c \"a b/main.kl.prelude.c\" -o \"main.kl.prelude.o\""
        );
    }
}

TEST_CASE( "link_command_links_the_runtime_after_the_objects", "[codegen_c][link]" )
{
    const std::vector<std::filesystem::path> objects = { "tmp/main.kl.o", "tmp/main.kl.prelude.o" };

    SECTION( "with flags" )
    {
        REQUIRE(
            keel::link_command( "clang", "-Wall -Werror", objects, "/rt/libkeel_rt.a", "out/main" ) ==
            "clang -fwrapv -fno-strict-aliasing -std=c11 -Wall -Werror \"tmp/main.kl.o\" \"tmp/main.kl.prelude.o\" "
            "\"/rt/libkeel_rt.a\" -o \"out/main\""
        );
    }

    SECTION( "without, leaving no gap" )
    {
        REQUIRE(
            keel::link_command( "cc", "", std::span( objects ).first( 1 ), "libkeel_rt.a", "main" ) ==
            "cc -fwrapv -fno-strict-aliasing -std=c11 \"tmp/main.kl.o\" \"libkeel_rt.a\" -o \"main\""
        );
    }
}

TEST_CASE( "link_command_links_the_users_c_after_the_runtime", "[codegen_c][link]" )
{
    const std::vector<std::filesystem::path> objects = { "main.kl.o" };
    const std::vector<std::string>           linked  = { "glue.c", "lib/libz.a", "-Llib", "-lm" };
    REQUIRE(
        keel::link_command( "cc", "", objects, "libkeel_rt.a", "main", linked ) ==
        "cc -fwrapv -fno-strict-aliasing -std=c11 \"main.kl.o\" \"libkeel_rt.a\" \"glue.c\" \"lib/libz.a\" \"-Llib\" "
        "\"-lm\" -o \"main\""
    );
}

TEST_CASE( "without_warning_flags_keeps_what_is_not_a_warning", "[codegen_c][link]" )
{
    REQUIRE( keel::without_warning_flags( "-O2 -Wall -Werror=conversion -pedantic-errors -I inc" ) == "-O2 -I inc" );
    REQUIRE(
        keel::without_warning_flags( "-Wl,-rpath,/x -Wa,--noexecstack -Wp,-DX" ) == "-Wl,-rpath,/x -Wa,--noexecstack -Wp,-DX"
    );
    REQUIRE( keel::without_warning_flags( "" ).empty() );
    REQUIRE( keel::without_warning_flags( "  -O2   -g " ) == "-O2 -g" );
}
#endif // ENABLE_UNIT_TESTS
