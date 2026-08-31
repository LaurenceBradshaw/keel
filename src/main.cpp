#include <fmt/core.h>
#include <fmt/format.h>
#include <cxxopts.hpp>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "common/version.h"
#include "lex/lexer.h"

// Excluded from the unit-test binary, which provides its own main() via Catch2.
#ifndef ENABLE_UNIT_TESTS
namespace
{

// Escapes the quotes and backslashes a string or char literal token contains, so every dump line
// stays unambiguously parseable when the golden runner diffs it.
std::string escape_for_dump( std::string_view text )
{
    std::string out;
    out.reserve( text.size() );

    for( const char c : text )
    {
        if( c == '"' || c == '\\' )
        {
            out.push_back( '\\' );
        }
        out.push_back( c );
    }

    return out;
}

} // namespace

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

    keel::Source_manager         sm;
    std::optional<keel::File_id> file_id = sm.load_file( std::filesystem::path( args["input"].as<std::string>() ) );

    if( !file_id )
    {
        fmt::print( stderr, "keelc: cannot open '{}'\n", args["input"].as<std::string>() );
        return 2;
    }

    // M0 lands here: read the file, lex it, parse it, and honour --dump-tokens / --dump-ast.
    keel::Interner           interner;
    keel::Diagnostics        diagnostics;
    std::vector<keel::Token> tokens = keel::lex( file_id.value(), sm, interner, diagnostics );

    if( args.count( "dump-tokens" ) )
    {
        for( const keel::Token& t : tokens )
        {
            keel::Span       span  = t.span;
            std::string_view text  = sm.text( span );
            keel::Line_col   start = sm.line_col( span.file, span.start );
            keel::Line_col   end   = sm.line_col( span.file, span.end );
            fmt::print(
                "{:<22} {:<12} \"{}\"\n",
                keel::token_kind_name( t.kind ),
                fmt::format( "{}:{}-{}:{}", start.line, start.col, end.line, end.col ),
                escape_for_dump( text )
            );
        }
    }

    if( args.count( "dump-ast" ) )
    {
        fmt::print( "AST dumping not yet implemented\n" );
    }

    diagnostics.render( sm, std::cerr );

    return diagnostics.has_errors() ? 1 : 0;
}
#endif // ENABLE_UNIT_TESTS
