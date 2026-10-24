// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_c/link.h"
#include <fmt/format.h>

namespace keel
{

Runtime find_runtime( const std::optional<std::string>& flag, const std::filesystem::path& installed )
{
    std::filesystem::path runtime_location = installed;
    if( flag.has_value() )
    {
        runtime_location = *flag;
    }

    // Verify that the runtime library exists at the specified location.
    std::error_code ec;
    if( !std::filesystem::exists( runtime_location, ec ) || !std::filesystem::is_regular_file( runtime_location, ec ) )
    {
        if( flag.has_value() )
        {
            return Runtime {
                runtime_location, fmt::format( "--runtime names '{}', which is not a file", runtime_location.string() )
            };
        }
        else
        {
            return Runtime {
                runtime_location,
                fmt::format(
                    "no runtime library at '{}' - install one there, or name one with --runtime <path>",
                    runtime_location.string()
                )
            };
        }
    }

    return Runtime { runtime_location, "" };
}

std::string link_command(
    std::string_view             compiler,
    std::string_view             cflags,
    const std::filesystem::path& source,
    const std::filesystem::path& runtime,
    const std::filesystem::path& executable,
    std::span<const std::string> linked
)
{
    std::string after_runtime;
    for( const std::string& word : linked )
    {
        after_runtime += fmt::format( " \"{}\"", word );
    }

    // §7.7: C's UB becomes Keel's UB without these.
    const std::string command = fmt::format(
        "{} -fwrapv -fno-strict-aliasing -std=c11{} \"{}\" \"{}\"{} -o \"{}\"",
        compiler,
        cflags.empty() ? std::string() : fmt::format( " {}", cflags ),
        source.string(),
        runtime.string(),
        after_runtime,
        executable.string()
    );
    return command;
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
    const std::filesystem::path named = dir.write( "named.a", "" );

    SECTION( "over an installed runtime" )
    {
        const std::filesystem::path installed = dir.write( "installed.a", "" );
        const keel::Runtime         runtime   = keel::find_runtime( named.string(), installed );

        REQUIRE( runtime.error.empty() );
        REQUIRE( runtime.path == named );
    }

    SECTION( "without consulting a missing installed one" )
    {
        const keel::Runtime runtime = keel::find_runtime( named.string(), dir.path / "absent.a" );

        REQUIRE( runtime.error.empty() );
        REQUIRE( runtime.path == named );
    }
}

TEST_CASE( "find_runtime_falls_back_to_the_installed_one", "[codegen_c][link]" )
{
    const keel::Temp_dir        dir;
    const std::filesystem::path installed = dir.write( "installed.a", "" );
    const keel::Runtime         runtime   = keel::find_runtime( std::nullopt, installed );

    REQUIRE( runtime.error.empty() );
    REQUIRE( runtime.path == installed );
}

TEST_CASE( "find_runtime_refuses_what_is_not_a_file", "[codegen_c][link]" )
{
    const keel::Temp_dir        dir;
    const std::filesystem::path absent = dir.path / "absent.a";

    SECTION( "a missing --runtime is named as the flag's" )
    {
        const keel::Runtime runtime = keel::find_runtime( absent.string(), dir.path / "installed.a" );

        REQUIRE( runtime.error == "--runtime names '" + absent.string() + "', which is not a file" );
    }

    SECTION( "a missing installed runtime says where it looked and what to do" )
    {
        const keel::Runtime runtime = keel::find_runtime( std::nullopt, absent );

        REQUIRE(
            runtime.error ==
            "no runtime library at '" + absent.string() + "' - install one there, or name one with --runtime <path>"
        );
    }

    SECTION( "a directory is not a library" )
    {
        const keel::Runtime runtime = keel::find_runtime( dir.path.string(), absent );

        REQUIRE( runtime.error == "--runtime names '" + dir.path.string() + "', which is not a file" );
    }
}

TEST_CASE( "link_command_links_the_runtime_after_the_source", "[codegen_c][link]" )
{
    SECTION( "with flags" )
    {
        REQUIRE(
            keel::link_command( "clang", "-Wall -Werror", "out/main.kl.c", "/rt/libkeel_rt.a", "out/main" ) ==
            "clang -fwrapv -fno-strict-aliasing -std=c11 -Wall -Werror \"out/main.kl.c\" \"/rt/libkeel_rt.a\" -o \"out/main\""
        );
    }

    SECTION( "without, leaving no gap" )
    {
        REQUIRE(
            keel::link_command( "cc", "", "main.kl.c", "libkeel_rt.a", "main" ) ==
            "cc -fwrapv -fno-strict-aliasing -std=c11 \"main.kl.c\" \"libkeel_rt.a\" -o \"main\""
        );
    }
}

TEST_CASE( "link_command_links_the_users_c_after_the_runtime", "[codegen_c][link]" )
{
    const std::vector<std::string> linked = { "glue.c", "lib/libz.a", "-Llib", "-lm" };
    REQUIRE(
        keel::link_command( "cc", "", "main.kl.c", "libkeel_rt.a", "main", linked ) ==
        "cc -fwrapv -fno-strict-aliasing -std=c11 \"main.kl.c\" \"libkeel_rt.a\" \"glue.c\" \"lib/libz.a\" \"-Llib\" "
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
