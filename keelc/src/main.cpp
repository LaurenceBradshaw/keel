// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
#include <unordered_map>
#include <vector>
#include "ast/dump.h"
#include "codegen_c/emit_kir.h"
#include "codegen_c/link.h"
#include "common/diagnostics.h"
#include "common/dump_util.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "common/version.h"
#include "front_end.h"
#include "ir/print.h"
#include "ir/verify.h"
#include "lex/lexer.h"
#include "sema/declarations.h"
#include "sema/names.h"

// Excluded from the unit-test binary, which provides its own main() via Catch2.
#ifndef ENABLE_UNIT_TESTS
namespace
{
// Where the pipeline stops, in pipeline order.
enum class Stop : keel::u8
{
    Tokens,
    Ast,
    Check,
    Kir,
    C,
    Executable
};
} // namespace

int main( int argc, char** argv )
{
    cxxopts::Options options( "keelc", "The Keel compiler" );

    // clang-format off
    options.add_options()
        ( "o,output",        "Write the executable here (default: the input's stem)", cxxopts::value<std::string>() )
        ( "dump-tokens",     "Print the token stream and stop" )
        ( "dump-ast",        "Print the parsed AST and stop" )
        ( "dump-kir",        "Print the lowered KIR and stop" )
        ( "emit-c",          "Print the generated C and stop" )
        ( "check",           "Run the front end and report diagnostics, emitting nothing" )
        ( "diagnostics",     "Report diagnostics as human (on stderr) or json (lines on stdout)", cxxopts::value<std::string>()->default_value( "human" ) )
        ( "names",           "With --diagnostics=json, also say what each name refers to" )
        ( "declarations",    "With --diagnostics=json, also list every declaration with its signature and doc" )
        ( "with-prelude",    "With --declarations, list the prelude's declarations too" )
        ( "package",         "A package and its directory, as name=<dir>; may be repeated", cxxopts::value<std::vector<std::string>>() )
        ( "W",               "Warnings: -W<name>, -Wno-<name>, -Werror, -Werror=<name>", cxxopts::value<std::vector<std::string>>() )
        ( "debug",           "Keep assert and overflow checks; without it an assert's condition is checked but never run" )
        ( "overflow-checks", "Panic when +, -, * or unary - overflows; on under --debug" )
        ( "runtime",         "Link this runtime library instead of the installed one", cxxopts::value<std::string>() )
        ( "l,library",       "Link this C library, as -l does; may be repeated", cxxopts::value<std::vector<std::string>>() )
        ( "L,library-path",  "Search this directory for -l libraries; may be repeated", cxxopts::value<std::vector<std::string>>() )
        ( "print-prelude",   "Print the prelude every program sees, and exit" )
        ( "v,version",       "Print version information and exit" )
        ( "h,help",          "Print usage and exit" )
        ( "input",           "Source file",                        cxxopts::value<std::string>() )
        ( "link",            "C to link: .c, .o or .a",            cxxopts::value<std::vector<std::string>>() );
    // clang-format on

    options.parse_positional( { "input", "link" } );
    options.positional_help( "<file.kl> [file.c|file.o|file.a ...]" );

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

    // An editor's only way to show it: `<prelude>` is no file on disk.
    if( args.count( "print-prelude" ) )
    {
        fmt::print( "{}", keel::prelude_source() );
        return 0;
    }

    if( !args.count( "input" ) )
    {
        fmt::print( stderr, "keelc: no input file\n" );
        fmt::print( stderr, "{}", options.help() );
        return 2;
    }

    // In command-line order, then the libraries, which follow everything that uses them.
    std::vector<std::string> linked;
    if( args.count( "link" ) )
    {
        for( const std::string& file : args["link"].as<std::vector<std::string>>() )
        {
            const std::string extension = std::filesystem::path( file ).extension().string();
            if( extension == ".kl" )
            {
                fmt::print(
                    stderr, "keelc: '{}' is a second .kl - name only the program's main file; its imports find the rest\n", file
                );
                return 2;
            }
            if( extension != ".c" && extension != ".o" && extension != ".a" )
            {
                fmt::print( stderr, "keelc: cannot link '{}' - name a .c, .o or .a\n", file );
                return 2;
            }
            linked.push_back( file );
        }
    }
    for( const auto& [option, flag] : { std::pair { "library-path", "-L" }, std::pair { "library", "-l" } } )
    {
        if( args.count( option ) )
        {
            for( const std::string& word : args[option].as<std::vector<std::string>>() )
            {
                linked.push_back( flag + word );
            }
        }
    }

    const std::string diagnostics_format = args["diagnostics"].as<std::string>();

    if( diagnostics_format != "human" && diagnostics_format != "json" )
    {
        fmt::print( stderr, "keelc: --diagnostics takes human or json, not '{}'\n", diagnostics_format );
        return 2;
    }

    for( const char* flag : { "names", "declarations" } )
    {
        if( args.count( flag ) && diagnostics_format != "json" )
        {
            fmt::print( stderr, "keelc: --{} needs --diagnostics=json\n", flag );
            return 2;
        }
    }

    if( args.count( "with-prelude" ) && !args.count( "declarations" ) )
    {
        fmt::print( stderr, "keelc: --with-prelude needs --declarations\n" );
        return 2;
    }

    Stop                                   stop    = Stop::Executable;
    const char*                            chose   = nullptr;
    constexpr std::pair<const char*, Stop> modes[] = {
        { "dump-tokens", Stop::Tokens },
        { "dump-ast", Stop::Ast },
        { "check", Stop::Check },
        { "dump-kir", Stop::Kir },
        { "emit-c", Stop::C },
    };

    for( const auto& [flag, mode] : modes )
    {
        if( !args.count( flag ) )
        {
            continue;
        }

        if( chose != nullptr )
        {
            fmt::print( stderr, "keelc: --{} and --{} cannot both be given\n", chose, flag );
            return 2;
        }
        stop  = mode;
        chose = flag;
    }

    keel::Interner             interner;
    std::vector<keel::Package> packages;

    if( args.count( "package" ) )
    {
        for( const std::string& spec : args["package"].as<std::vector<std::string>>() )
        {
            const std::size_t equals = spec.find( '=' );

            if( equals == std::string::npos || equals == 0 || equals + 1 == spec.size() )
            {
                fmt::print( stderr, "keelc: --package takes name=<dir>, not '{}'\n", spec );
                return 2;
            }

            const std::string name = spec.substr( 0, equals );

            if( const std::optional<std::string> error = keel::package_name_error( name, interner ) )
            {
                fmt::print( stderr, "keelc: --package {}\n", *error );
                return 2;
            }

            packages.push_back( keel::Package { .name = name, .root = spec.substr( equals + 1 ) } );
        }
    }

    // Asked only of a build that links, and before anything is written, so a missing runtime never reaches cc.
    keel::Runtime runtime;

    if( stop == Stop::Executable )
    {
        const std::optional<std::string> flag =
            args.count( "runtime" ) ? std::optional( args["runtime"].as<std::string>() ) : std::nullopt;

        runtime = keel::find_runtime( flag, KEEL_INSTALLED_RUNTIME );

        if( !runtime.error.empty() )
        {
            fmt::print( stderr, "keelc: {}\n", runtime.error );
            return 2;
        }
    }

    keel::Source_manager         sm;
    std::optional<keel::File_id> file_id = sm.load_file( std::filesystem::path( args["input"].as<std::string>() ) );

    if( !file_id )
    {
        fmt::print( stderr, "keelc: cannot open '{}'\n", args["input"].as<std::string>() );
        return 2;
    }

    keel::Diagnostics                          diagnostics;
    keel::Literal_pool                         literals;
    std::vector<keel::Name>                    names;
    std::vector<keel::Declaration>             declarations;
    std::unordered_map<keel::u32, std::string> package_docs;

    if( args.count( "W" ) )
    {
        for( const std::string& flag : args["W"].as<std::vector<std::string>>() )
        {
            if( !diagnostics.apply_warning_flag( flag ) )
            {
                const std::size_t prefix = flag.starts_with( "error=" ) ? 6 : flag.starts_with( "no-" ) ? 3 : 0;
                fmt::print( stderr, "keelc: unknown warning '{}' in -W{}\n", flag.substr( prefix ), flag );
                return 2;
            }
        }
    }

    // Reporting is the same wherever we stop, and each --dump flag stops after its own phase.
    const auto finish = [&]() -> int
    {
        if( diagnostics_format == "json" )
        {
            diagnostics.render_json( sm, std::cout );
            keel::render_names_json( sm, names, std::cout );

            if( args.count( "declarations" ) )
            {
                keel::render_declarations_json( sm, interner, package_docs, declarations, std::cout );
            }
            return diagnostics.has_errors() ? 1 : 0;
        }

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

    if( stop == Stop::Tokens )
    {
        std::vector<keel::Token> tokens = keel::lex( file_id.value(), sm, interner, literals, diagnostics );
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

    // The tree alone: a debugging view, not a pipeline stage.
    if( stop == Stop::Ast )
    {
        const keel::Program prog = keel::load_program( file_id.value(), sm, interner, literals, diagnostics, packages );
        keel::dump_ast( prog.ast, sm, interner, std::cout, prog.modules.prelude_file() );
        return finish();
    }

    const bool debug = args.count( "debug" ) > 0;

    // Every other mode needs the whole front end; --check stops after it, leaving no artefacts behind.
    keel::Front_end front = keel::run_front_end(
        file_id.value(),
        keel::Stage::Elaborated,
        keel::Build { .asserts = debug, .overflow_checks = debug || args.count( "overflow-checks" ) > 0, .optimise = !debug },
        sm,
        interner,
        literals,
        diagnostics,
        packages
    );
    const keel::Program&               prog      = front.program;
    const keel::Ast&                   ast       = prog.ast;
    keel::Types&                       types     = front.types;
    const std::vector<keel::Function>& functions = front.functions;

    if( args.count( "names" ) )
    {
        names = keel::collect_names( ast, front.resolution, types, sm, interner );

        // The prelude is not on disk for an editor to open; a use of it still names it.
        std::erase_if( names, [&]( const keel::Name& name ) { return name.span.file == prog.modules.prelude_file(); } );
    }

    if( args.count( "declarations" ) )
    {
        declarations = keel::collect_declarations( ast, front.resolution, types, sm, interner );

        // The prelude belongs to no package, so only a doc tool writing its page asks for it.
        if( !args.count( "with-prelude" ) )
        {
            std::erase_if(
                declarations, [&]( const keel::Declaration& decl ) { return decl.span.file == prog.modules.prelude_file(); }
            );
        }
        package_docs = prog.package_docs;
    }

    if( diagnostics.has_errors() || stop == Stop::Check )
    {
        return finish();
    }

    // Additive: the C path below is untouched, and nothing consumes KIR yet. This is what makes
    // each step of the lowerer visible as it lands.
    if( stop == Stop::Kir )
    {
        bool               well_formed = true;
        const keel::Halves halves( ast, types.table(), prog.modules );

        // The program's half: what its own C file holds.
        for( const keel::Function& function : functions )
        {
            if( halves.of( function ) == keel::C_part::Prelude )
            {
                continue;
            }

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

    // Named after the input, like the .c that includes it, so two programs built side by side do not
    // share one: its node ids are numbered after the program's.
    const std::string prelude_name =
        std::filesystem::path( args["input"].as<std::string>() ).filename().string() + ".prelude.c";

    const std::string generated = keel::emit_c_from_kir(
        functions, ast, types, literals, sm, interner, prog.modules, keel::C_part::Program, prelude_name
    );

    if( stop == Stop::C )
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

    // Named after the input but written beside the executable - `foo.kl.c` says what it came from,
    // and both artifacts land wherever `-o` points rather than in the source tree.
    const std::filesystem::path generated_path =
        executable.parent_path() / ( std::filesystem::path( args["input"].as<std::string>() ).filename().string() + ".c" );

    const std::filesystem::path prelude_c_path = executable.parent_path() / prelude_name;

    const std::pair<std::filesystem::path, std::string> outputs[] = {
        { generated_path, generated },
        { prelude_c_path,
          keel::emit_c_from_kir( functions, ast, types, literals, sm, interner, prog.modules, keel::C_part::Prelude ) },
    };

    for( const auto& [path, text] : outputs )
    {
        std::ofstream out( path );

        if( !out )
        {
            fmt::print( stderr, "keelc: cannot write {}\n", path.string() );
            return 1;
        }

        out << text;
    }

    // $CC if it is set, so a cross compiler or a specific clang can be selected without a flag.
    const char* const configured = std::getenv( "CC" );
    const std::string compiler   = configured != nullptr && *configured != '\0' ? configured : "cc";

    // $CFLAGS without its warnings, then $KEEL_C_WARNINGS as given: only keelc's tests ask the C
    // compiler to judge the C.
    const char* const extra  = std::getenv( "CFLAGS" );
    std::string       cflags = extra != nullptr ? keel::without_warning_flags( extra ) : "";

    const char* const c_warnings = std::getenv( "KEEL_C_WARNINGS" );
    if( c_warnings != nullptr && *c_warnings != '\0' )
    {
        cflags += cflags.empty() ? "" : " ";
        cflags += c_warnings;
    }

    const std::string command = keel::link_command( compiler, cflags, generated_path, runtime.path, executable, linked );

    if( const int status = std::system( command.c_str() ); status != 0 )
    {
        // Without the user's own C, a failure here is a compiler bug: the program type-checked, so
        // the C it produced should always compile. The path is named so the output can be inspected.
        // With it, the user's C or a missing symbol is the likelier cause.
        if( linked.empty() )
        {
            fmt::print( stderr, "keelc: the generated C failed to compile - see {}\n", generated_path.string() );
        }
        else
        {
            fmt::print( stderr, "keelc: compiling or linking the C failed - the generated C is {}\n", generated_path.string() );
        }
        return 1;
    }

    return finish();
}
#endif // ENABLE_UNIT_TESTS
