#include <fmt/core.h>
#include <fmt/format.h>
#include <cstdlib>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include "ast/dump.h"
#include "check/drop_flags.h"
#include "check/move_check.h"
#include "codegen_c/emit_kir.h"
#include "common/diagnostics.h"
#include "common/dump_util.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "common/version.h"
#include "ir/lower.h"
#include "ir/print.h"
#include "ir/verify.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

// Excluded from the unit-test binary, which provides its own main() via Catch2.
#ifndef ENABLE_UNIT_TESTS
namespace
{

// check_moves reports spans and a Local_id; the wording is the driver's, because this is the only
// place with an Interner to turn that local into a name. Diagnostics carries one span and a help
// string rather than a second underlined snippet, so the move site becomes a line:col in the help -
// the shape the resolver's previous-declaration note already uses.
void report_move_errors(
    const std::vector<keel::Function>& functions,
    const keel::Source_manager&        sm,
    const keel::Interner&              interner,
    keel::Diagnostics&                 diagnostics
)
{
    for( const keel::Function& function : functions )
    {
        for( const keel::Move_error& error : keel::check_moves( function ) )
        {
            const keel::Symbol_id name = function.locals[error.local.v].name;

            // Only a named local can be moved, so a temporary never reaches here.
            assert( name.is_valid() && "a move error names a local the author wrote" );

            const keel::Line_col at = sm.line_col( error.moved.file, error.moved.start );

            // The two must read differently: `maybe` is the compiler refusing an ambiguity rather
            // than reporting a certainty, and that is the whole reason the state exists.
            diagnostics.error(
                error.use,
                error.maybe ? fmt::format( "`{}` may already have been moved", interner.text( name ) )
                            : fmt::format( "`{}` is used after it was moved", interner.text( name ) ),
                error.maybe ? fmt::format( "moved at {}:{} on some path to here", at.line, at.col )
                            : fmt::format( "moved at {}:{}", at.line, at.col )
            );
        }
    }
}

} // namespace

int main( int argc, char** argv )
{
    cxxopts::Options options( "keelc", "The Keel compiler" );

    // clang-format off
    options.add_options()
        ( "o,output",      "Write the executable here (default: the input's stem)", cxxopts::value<std::string>() )
        ( "dump-tokens",   "Print the token stream and stop" )
        ( "dump-ast",      "Print the parsed AST and stop" )
        ( "dump-kir",      "Print the lowered KIR and stop" )
        ( "emit-c",        "Print the generated C and stop" )
        ( "check",         "Run the front end and report diagnostics, emitting nothing" )
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

    keel::Interner           interner;
    keel::Diagnostics        diagnostics;
    keel::Literals           literals;
    std::vector<keel::Token> tokens = keel::lex( file_id.value(), sm, interner, literals, diagnostics );

    // Reporting is the same wherever we stop, and each --dump flag stops after its own phase.
    const auto finish = [&]() -> int
    {
        diagnostics.render( sm, std::cerr, keel::colour_supported() );

        if( !diagnostics.has_errors() )
        {
            return 0;
        }

        // A count at the end, so it is obvious whether output was truncated by a pager.
        const std::size_t count = diagnostics.error_count();
        fmt::print( stderr, "\n{} error{} generated\n", count, count == 1 ? "" : "s" );
        return 1;
    };

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
                keel::escape_for_dump( text )
            );
        }

        return finish();
    }

    // Parsing is part of compiling, not a debug feature: --dump-ast only controls output.
    const keel::Ast ast = keel::parse( tokens, sm, diagnostics );

    if( args.count( "dump-ast" ) )
    {
        keel::dump_ast( ast, sm, interner, std::cout );
        return finish();
    }

    // Resolution is part of compiling, not a debug feature - same reasoning as parsing.
    const keel::Resolution resolution = keel::resolve( ast, sm, interner, diagnostics );

    // Type checking is part of compiling too - same reasoning as parsing and resolution.
    const keel::Types types = keel::type_check( ast, resolution, literals, sm, interner, diagnostics );

    // Nothing is emitted for a program that did not check: the emitter takes no Diagnostics
    // because by here there is nothing left for it to object to.
    if( diagnostics.has_errors() )
    {
        return finish();
    }

    // Lowered once, for everything downstream: the move check, --dump-kir and the emitter all read
    // the same functions rather than each lowering a copy of its own.
    std::vector<keel::Function> functions = keel::lower( ast, resolution, types, literals, interner );

    // Move checking is part of the front end, not of emission: --check is what an editor wants, and
    // an editor wants use-after-move underlined. Which is why this runs above that early return
    // rather than beside the emitter.
    report_move_errors( functions, sm, interner, diagnostics );

    if( diagnostics.has_errors() )
    {
        return finish();
    }

    // Built once rather than per function: the two literals are interned, so asking for them each
    // time would add one entry per function that moves anything.
    const keel::Flag_vocabulary flag_vocabulary {
        .bool_type     = types.table().builtin( keel::Type_kind::Bool ),
        .false_literal = literals.add_integer( 0 ),
        .true_literal  = literals.add_integer( 1 )
    };

    for( keel::Function& function : functions )
    {
        keel::elaborate_drops( function, flag_vocabulary );
    }

    // Everything the front end can say has been said by here. --check is what an editor or a
    // diagnostics-only test wants, and it leaves no artefacts behind.
    if( args.count( "check" ) )
    {
        return finish();
    }

    // Additive: the C path below is untouched, and nothing consumes KIR yet. This is what makes
    // each step of the lowerer visible as it lands (PLAN §3.3).
    if( args.count( "dump-kir" ) )
    {
        bool well_formed = true;

        for( const keel::Function& function : functions )
        {
            std::cout << keel::print( function, ast, types.table(), literals, interner );

            // A malformed function is a bug in the lowerer, not in the program - so it goes to
            // stderr as an internal error rather than through Diagnostics. Running it here is what
            // puts every fixture in the corpus behind the verifier.
            for( const std::string& problem : keel::verify( function ) )
            {
                fmt::print( stderr, "keelc: internal error: malformed KIR: {}\n", problem );
                well_formed = false;
            }
        }

        if( !well_formed )
        {
            return 1;
        }

        return finish();
    }

    const std::string generated = keel::emit_c_from_kir( functions, ast, types, literals, sm, interner );

    if( args.count( "emit-c" ) )
    {
        std::cout << generated;
        return finish();
    }

    // The .c is written beside the executable and kept, not deleted. Its #line directives point
    // back at the .kl, so it is the thing to read when the output is wrong - and it is what
    // --emit-c prints for the golden tests.
    const std::filesystem::path executable = args.count( "output" )
                                                 ? std::filesystem::path( args["output"].as<std::string>() )
                                                 : std::filesystem::path( args["input"].as<std::string>() ).stem();

    // Named after the input rather than the output - `foo.kl.c` says what it came from, and is the
    // shape .gitignore already expects.
    const std::filesystem::path generated_path = args["input"].as<std::string>() + ".c";

    {
        std::ofstream out( generated_path );

        if( !out )
        {
            fmt::print( stderr, "keelc: cannot write {}\n", generated_path.string() );
            return 1;
        }

        out << generated;
    }

    // $CC if it is set, so a cross compiler or a specific clang can be selected without a flag.
    const char* const configured = std::getenv( "CC" );
    const std::string compiler   = configured != nullptr && *configured != '\0' ? configured : "cc";

    // §7.7: C's UB becomes Keel's UB without these.
    const std::string command = fmt::format(
        "{} -fwrapv -fno-strict-aliasing -std=c11 \"{}\" -o \"{}\"", compiler, generated_path.string(), executable.string()
    );

    if( const int status = std::system( command.c_str() ); status != 0 )
    {
        // A failure here is a compiler bug, not a user error: the program type-checked, so the C
        // it produced should always compile. The path is named so the output can be inspected.
        fmt::print( stderr, "keelc: the generated C failed to compile - see {}\n", generated_path.string() );
        return 1;
    }

    return finish();
}
#endif // ENABLE_UNIT_TESTS
