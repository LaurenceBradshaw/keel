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
    const std::filesystem::path& executable
)
{
    // §7.7: C's UB becomes Keel's UB without these.
    const std::string command = fmt::format(
        "{} -fwrapv -fno-strict-aliasing -std=c11{} \"{}\" \"{}\" -o \"{}\"",
        compiler,
        cflags.empty() ? std::string() : fmt::format( " {}", cflags ),
        source.string(),
        runtime.string(),
        executable.string()
    );
    return command;
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
#endif // ENABLE_UNIT_TESTS
