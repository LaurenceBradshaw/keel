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
#include "check/drop_flags.h"
#include "check/report.h"
#include "codegen_c/emit_kir.h"
#include "codegen_c/link.h"
#include "common/diagnostics.h"
#include "common/dump_util.h"
#include "common/imports.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "common/version.h"
#include "ir/lower.h"
#include "ir/print.h"
#include "ir/simplify.h"
#include "ir/verify.h"
#include "lex/lexer.h"
#include "parse/loader.h"
#include "sema/declarations.h"
#include "sema/names.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

// Excluded from the unit-test binary, which provides its own main() via Catch2.
#ifndef ENABLE_UNIT_TESTS
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
        ( "diagnostics",   "Report diagnostics as human (on stderr) or json (lines on stdout)", cxxopts::value<std::string>()->default_value( "human" ) )
        ( "names",         "With --diagnostics=json, also say what each name refers to" )
        ( "declarations",  "With --diagnostics=json, also list every declaration with its signature and doc" )
        ( "with-prelude",  "With --declarations, list the prelude's declarations too" )
        ( "package",       "A package and its directory, as name=<dir>; may be repeated", cxxopts::value<std::vector<std::string>>() )
        ( "runtime",       "Link this runtime library instead of the installed one", cxxopts::value<std::string>() )
        ( "print-prelude", "Print the prelude every program sees, and exit" )
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

            if( name == "prelude" )
            {
                fmt::print( stderr, "keelc: --package cannot name a package 'prelude', which is the prelude's own\n" );
                return 2;
            }

            if( interner.is_keyword( interner.find( name ) ) )
            {
                fmt::print( stderr, "keelc: --package name '{}' is a keyword\n", name );
                return 2;
            }

            if( !keel::is_identifier( name ) )
            {
                fmt::print( stderr, "keelc: --package name '{}' is not an identifier\n", name );
                return 2;
            }

            packages.push_back( keel::Package { .name = name, .root = spec.substr( equals + 1 ) } );
        }
    }

    // Asked only of a build that links, and before anything is written, so a missing runtime never reaches cc.
    const bool links = !args.count( "check" ) && !args.count( "emit-c" ) && !args.count( "dump-tokens" ) &&
                       !args.count( "dump-ast" ) && !args.count( "dump-kir" );

    keel::Runtime runtime;

    if( links )
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

    if( args.count( "dump-tokens" ) )
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

    // Parsing is part of compiling, not a debug feature: --dump-ast only controls output.
    const keel::Program prog = keel::load_program( file_id.value(), sm, interner, literals, diagnostics, packages );
    const keel::Ast&    ast  = prog.ast;

    if( args.count( "dump-ast" ) )
    {
        keel::dump_ast( ast, sm, interner, std::cout, prog.imports.prelude_file() );
        return finish();
    }

    // Resolution is part of compiling, not a debug feature - same reasoning as parsing.
    const keel::Resolution resolution = keel::resolve( ast, sm, interner, diagnostics, prog.imports );

    // Type checking is part of compiling too - same reasoning as parsing and resolution.
    keel::Types types = keel::type_check( ast, resolution, literals, sm, interner, diagnostics );

    // After checking, which alone binds a field or method named after a `.`.
    if( args.count( "names" ) )
    {
        names = keel::collect_names( ast, resolution, types, sm, interner );

        // The prelude is not on disk for an editor to open; a use of it still names it.
        std::erase_if( names, [&]( const keel::Name& name ) { return name.span.file == prog.imports.prelude_file(); } );
    }

    if( args.count( "declarations" ) )
    {
        declarations = keel::collect_declarations( ast, resolution, types, sm, interner );

        // The prelude belongs to no package, so only a doc tool writing its page asks for it.
        if( !args.count( "with-prelude" ) )
        {
            std::erase_if(
                declarations, [&]( const keel::Declaration& decl ) { return decl.span.file == prog.imports.prelude_file(); }
            );
        }
        package_docs = prog.package_docs;
    }

    // Nothing is emitted for a program that did not check: the emitter takes no Diagnostics
    // because by here there is nothing left for it to object to.
    if( diagnostics.has_errors() )
    {
        return finish();
    }

    // Lowered once, for everything downstream: the move check, --dump-kir and the emitter all read
    // the same functions rather than each lowering a copy of its own.
    std::vector<keel::Function> functions = keel::lower( ast, resolution, types, literals );

    for( keel::Function& function : functions )
    {
        keel::simplify( function, literals );
    }

    // Move checking is part of the front end, not of emission: --check is what an editor wants, and
    // an editor wants use-after-move underlined. Which is why this runs above that early return
    // rather than beside the emitter.
    keel::report_dataflow_errors( functions, ast, sm, interner, types, diagnostics );

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
    // each step of the lowerer visible as it lands.
    if( args.count( "dump-kir" ) )
    {
        bool well_formed = true;

        for( const keel::Function& function : functions )
        {
            if( function.declaration.is_valid() && ast.span( function.declaration ).file == prog.imports.prelude_file() )
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
        functions, ast, types, literals, sm, interner, prog.imports, keel::C_part::Program, prelude_name
    );

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

    // Named after the input but written beside the executable - `foo.kl.c` says what it came from,
    // and both artifacts land wherever `-o` points rather than in the source tree.
    const std::filesystem::path generated_path =
        executable.parent_path() / ( std::filesystem::path( args["input"].as<std::string>() ).filename().string() + ".c" );

    const std::filesystem::path prelude_c_path = executable.parent_path() / prelude_name;

    const std::pair<std::filesystem::path, std::string> outputs[] = {
        { generated_path, generated },
        { prelude_c_path,
          keel::emit_c_from_kir( functions, ast, types, literals, sm, interner, prog.imports, keel::C_part::Prelude ) },
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

    // $CFLAGS after keelc's own, as a build would pass them.
    const char* const extra  = std::getenv( "CFLAGS" );
    const std::string cflags = extra != nullptr ? extra : "";

    const std::string command = keel::link_command( compiler, cflags, generated_path, runtime.path, executable );

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
