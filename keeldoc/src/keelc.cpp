// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "keelc.h"
#include <fmt/format.h>
#include <sys/wait.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace keeldoc
{
namespace
{

// `text` as one word to the shell.
std::string shell_quote( std::string_view text )
{
    std::string quoted = "'";
    for( const char c : text )
    {
        quoted += c == '\'' ? std::string( "'\\''" ) : std::string( 1, c );
    }

    return quoted + "'";
}

} // namespace

std::vector<std::string> package_modules( const std::string& name, const std::filesystem::path& dir )
{
    std::vector<std::string> modules;

    std::error_code ec;
    for( const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator( dir, ec ) )
    {
        const std::filesystem::path& path = entry.path();
        if( entry.is_regular_file() && path.extension() == ".kl" && path.filename() != "packageinfo.kl" )
        {
            modules.push_back( name + "::" + path.stem().string() );
        }
    }

    std::ranges::sort( modules );
    return modules;
}

std::vector<Record>
run_keelc( const std::filesystem::path& keelc, const std::string& name, const std::filesystem::path& dir, std::string& error )
{
    const std::vector<std::string> modules = package_modules( name, dir );
    if( modules.empty() )
    {
        error = fmt::format( "'{}' holds no module", dir.string() );
        return {};
    }

    std::string root_dir = ( std::filesystem::temp_directory_path() / "keeldoc_XXXXXX" ).string();
    if( mkdtemp( root_dir.data() ) == nullptr )
    {
        error = "cannot make a temporary directory";
        return {};
    }

    const std::filesystem::path root = std::filesystem::path( root_dir ) / "root.kl";
    {
        std::ofstream out( root );
        for( const std::string& module : modules )
        {
            out << "import " << module << ";\n";
        }
    }

    const std::string command = fmt::format(
        "{} --check --diagnostics=json --declarations --package {} {} 2>&1",
        shell_quote( keelc.string() ),
        shell_quote( name + "=" + dir.string() ),
        shell_quote( root.string() )
    );

    std::vector<Record> records;
    std::string         unparsed;

    if( FILE* pipe = popen( command.c_str(), "r" ); pipe != nullptr )
    {
        std::string            line;
        std::array<char, 4096> buffer {};
        while( std::fgets( buffer.data(), static_cast<int>( buffer.size() ), pipe ) != nullptr )
        {
            line += buffer.data();
            if( line.ends_with( '\n' ) )
            {
                line.pop_back();
                if( std::optional<Record> record = parse_record( line ) )
                {
                    records.push_back( std::move( *record ) );
                }
                else
                {
                    unparsed += line + "\n";
                }
                line.clear();
            }
        }

        const int status = pclose( pipe );
        if( records.empty() )
        {
            error = fmt::format(
                "'{}' gave no records (status {}){}",
                keelc.string(),
                WEXITSTATUS( status ),
                unparsed.empty() ? "" : ":\n" + unparsed
            );
        }
    }
    else
    {
        error = fmt::format( "cannot run '{}'", keelc.string() );
    }

    std::error_code ec;
    std::filesystem::remove_all( root_dir, ec );
    return records;
}

} // namespace keeldoc

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keeldoc
{
namespace
{

TEST_CASE( "keelc_imports_every_module_but_packageinfo", "[keelc]" )
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "keeldoc_test_modules";
    std::filesystem::remove_all( dir );
    std::filesystem::create_directories( dir / "nested" );
    for( const char* file : { "b.kl", "a.kl", "packageinfo.kl", "notes.txt" } )
    {
        std::ofstream( dir / file ) << "\n";
    }

    REQUIRE( package_modules( "geo", dir ) == std::vector<std::string> { "geo::a", "geo::b" } );

    std::filesystem::remove_all( dir );
}

TEST_CASE( "keelc_says_when_it_cannot_run", "[keelc]" )
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "keeldoc_test_run";
    std::filesystem::remove_all( dir );
    std::filesystem::create_directories( dir );
    std::ofstream( dir / "a.kl" ) << "\n";

    std::string error;
    REQUIRE( run_keelc( dir / "no-such-keelc", "geo", dir, error ).empty() );
    REQUIRE( error.starts_with( "'" + ( dir / "no-such-keelc" ).string() + "' gave no records" ) );

    REQUIRE( run_keelc( "keelc", "geo", dir / "missing", error ).empty() );
    REQUIRE( error == "'" + ( dir / "missing" ).string() + "' holds no module" );

    std::filesystem::remove_all( dir );
}

} // namespace
} // namespace keeldoc
#endif // ENABLE_UNIT_TESTS
