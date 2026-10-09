// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <fmt/core.h>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "keelc.h"
#include "package.h"
#include "pages.h"

// Excluded from the unit-test binary, which provides its own main() via Catch2.
#ifndef ENABLE_UNIT_TESTS
namespace
{

// The keelc beside this binary, which is where a build puts both.
std::filesystem::path default_keelc()
{
    std::error_code             ec;
    const std::filesystem::path self = std::filesystem::read_symlink( "/proc/self/exe", ec );
    return ec ? std::filesystem::path( "keelc" ) : self.parent_path() / "keelc";
}

} // namespace

int main( int argc, char** argv )
{
    cxxopts::Options options( "keeldoc", "Writes a Keel package's documentation as HTML pages" );

    // clang-format off
    options.add_options()
        ( "o,output",  "Write the pages into this directory",                cxxopts::value<std::string>() )
        ( "keelc",     "The keelc to run (default: the one beside keeldoc)", cxxopts::value<std::string>() )
        ( "h,help",    "Print usage and exit" )
        ( "package",   "The package, as name=<dir>",                         cxxopts::value<std::string>() );
    // clang-format on

    options.parse_positional( { "package" } );
    options.positional_help( "<name>=<dir>" );

    cxxopts::ParseResult args;
    try
    {
        args = options.parse( argc, argv );
    }
    catch( const cxxopts::exceptions::exception& e )
    {
        fmt::print( stderr, "keeldoc: {}\n", e.what() );
        return 2;
    }

    if( args.count( "help" ) )
    {
        fmt::print( "{}", options.help() );
        return 0;
    }

    if( !args.count( "package" ) || !args.count( "output" ) )
    {
        fmt::print( stderr, "keeldoc: needs a package and --output\n" );
        fmt::print( stderr, "{}", options.help() );
        return 2;
    }

    const std::string package = args["package"].as<std::string>();
    const std::size_t equals  = package.find( '=' );
    if( equals == 0 || equals == std::string::npos || equals + 1 == package.size() )
    {
        fmt::print( stderr, "keeldoc: the package is name=<dir>, not '{}'\n", package );
        return 2;
    }

    const std::string           name = package.substr( 0, equals );
    const std::filesystem::path dir  = package.substr( equals + 1 );
    const std::filesystem::path keelc =
        args.count( "keelc" ) ? std::filesystem::path( args["keelc"].as<std::string>() ) : default_keelc();

    std::string                        error;
    const std::vector<keeldoc::Record> records = keeldoc::run_keelc( keelc, name, dir, error );
    if( !error.empty() )
    {
        fmt::print( stderr, "keeldoc: {}\n", error );
        return 1;
    }

    const std::string prelude = keeldoc::print_prelude( keelc, error );
    if( !error.empty() )
    {
        fmt::print( stderr, "keeldoc: {}\n", error );
        return 1;
    }

    std::vector<std::string> errors;
    const keeldoc::Package   documented = keeldoc::build_package( name, dir, records, prelude, errors );
    for( const std::string& message : errors )
    {
        fmt::print( stderr, "{}\n", message );
    }

    if( !errors.empty() )
    {
        fmt::print(
            stderr,
            "keeldoc: keelc reported {} error{}, so no pages were written\n",
            errors.size(),
            errors.size() == 1 ? "" : "s"
        );
        return 1;
    }

    for( const keeldoc::Module& module : documented.modules )
    {
        if( keeldoc::page_of( module ) == "index.html" || keeldoc::page_of( module ) == keeldoc::k_prelude_page )
        {
            fmt::print( stderr, "keeldoc: a module named `{}` would overwrite another page\n", module.name );
            return 1;
        }
    }

    const std::filesystem::path output = args["output"].as<std::string>();
    std::error_code             ec;
    std::filesystem::create_directories( output, ec );

    for( const auto& [file, contents] : keeldoc::render_pages( documented ) )
    {
        std::ofstream out( output / file, std::ios::binary );
        out.write( contents.data(), static_cast<std::streamsize>( contents.size() ) );
        if( !out )
        {
            fmt::print( stderr, "keeldoc: cannot write '{}'\n", ( output / file ).string() );
            return 1;
        }
    }

    return 0;
}
#endif
