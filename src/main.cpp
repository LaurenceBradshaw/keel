#include "common/version.h"

#include <fmt/core.h>
#include <cxxopts.hpp>

#include <filesystem>
#include <string>

// Excluded from the unit-test binary, which provides its own main() via Catch2.
#ifndef ENABLE_UNIT_TESTS
int main( int argc, char** argv )
{
    cxxopts::Options options( "keelc", "The Keel compiler" );

    // clang-format off
    options.add_options()
        ( "o,output",      "Write the generated C to this path", cxxopts::value<std::string>() )
        ( "dump-tokens",   "Print the token stream and stop" )
        ( "dump-ast",      "Print the parsed AST and stop" )
        ( "v,version",     "Print version information and exit" )
        ( "h,help",        "Print usage and exit" )
        ( "input",         "Source file",                        cxxopts::value<std::string>() );
    // clang-format on

    options.parse_positional( { "input" } );
    options.positional_help( "<file.kl>" );

    cxxopts::ParseResult args;
    try
    {
        args = options.parse( argc, argv );
    }
    catch( const cxxopts::exceptions::exception& e )
    {
        fmt::print( stderr, "keelc: {}\n", e.what() );
        return 2;
    }

    if( args.count( "help" ) )
    {
        fmt::print( "{}", options.help() );
        return 0;
    }

    if( args.count( "version" ) )
    {
        fmt::print( "{}\n", keel::version_string() );
        return 0;
    }

    if( !args.count( "input" ) )
    {
        fmt::print( stderr, "keelc: no input file\n" );
        fmt::print( stderr, "{}", options.help() );
        return 2;
    }

    const std::filesystem::path input = args["input"].as<std::string>();
    if( !std::filesystem::exists( input ) )
    {
        fmt::print( stderr, "keelc: cannot open '{}': no such file\n", input.string() );
        return 2;
    }

    // M0 lands here: read the file, lex it, parse it, and honour --dump-tokens / --dump-ast.
    fmt::print( stderr, "keelc: front end not implemented yet (would compile '{}')\n", input.string() );
    return 1;
}
#endif // ENABLE_UNIT_TESTS
