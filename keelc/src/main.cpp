// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <fmt/core.h>
#include <fmt/format.h>
#include <algorithm>
#include <cstdlib>
#include <cxxopts.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "ast/dump.h"
#include "codegen_c/emit_kir.h"
#include "codegen_c/link.h"
#include "codegen_c/spelling.h"
#include "common/diagnostics.h"
#include "common/dump_util.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "common/temp_dir.h"
#include "common/version.h"
#include "front_end.h"
#include "ir/print.h"
#include "ir/verify.h"
#include "lex/lexer.h"
#include "object/object.h"
#include "parse/parser.h"
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
    Object,
    Executable,
    Imports
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
        ( "c,object",        "Compile the input as a module into a .klo" )
        ( "check",           "Run the front end and report diagnostics, emitting nothing" )
        ( "imports",         "Print the input's imports as written, one per line, and stop" )
        ( "diagnostics",     "Report diagnostics as human (on stderr) or json (lines on stdout)", cxxopts::value<std::string>()->default_value( "human" ) )
        ( "names",           "With --diagnostics=json, also say what each name refers to" )
        ( "declarations",    "With --diagnostics=json, also list every declaration with its signature and doc" )
        ( "with-prelude",    "With --declarations, list the prelude's declarations too" )
        ( "package",         "A package and its directory, as name=<dir>; may be repeated", cxxopts::value<std::vector<std::string>>() )
        ( "W",               "Warnings: -W<name>, -Wno-<name>, -Werror, -Werror=<name>", cxxopts::value<std::vector<std::string>>() )
        ( "debug",           "Keep assert and overflow checks; without it an assert's condition is checked but never run" )
        ( "overflow-checks", "Panic when +, -, * or unary - overflows; on under --debug" )
        ( "runtime",         "Link this runtime library instead of the one keelc carries", cxxopts::value<std::string>() )
        ( "l,library",       "Link this C library, as -l does; may be repeated", cxxopts::value<std::vector<std::string>>() )
        ( "L,library-path",  "Search this directory for -l libraries; may be repeated", cxxopts::value<std::vector<std::string>>() )
        ( "print-prelude",   "Print the prelude every program sees, and exit" )
        ( "print-config",    "Print this keelc's build id and package folders, and exit" )
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

    if( !args.count( "input" ) && !args.count( "print-config" ) )
    {
        fmt::print( stderr, "keelc: no input file\n" );
        fmt::print( stderr, "{}", options.help() );
        return 2;
    }

    // In command-line order, then the libraries, which follow everything that uses them.
    std::vector<std::string>           linked;
    std::vector<std::filesystem::path> klos;
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
            if( extension == ".klo" )
            {
                klos.push_back( file );
                continue;
            }
            if( extension != ".c" && extension != ".o" && extension != ".a" )
            {
                fmt::print( stderr, "keelc: cannot link '{}' - name a .c, .o, .a or .klo\n", file );
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
        { "object", Stop::Object },
        { "imports", Stop::Imports },
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

    // kl beside keelc unless --package names another.
    auto kl = std::ranges::find( packages, std::string( "kl" ), &keel::Package::name );
    if( kl == packages.end() && std::filesystem::is_directory( keel::library_folder() / "kl" ) )
    {
        packages.push_back( keel::Package { .name = "kl", .root = keel::library_folder() / "kl" } );
        kl = packages.end() - 1;
    }

    // After --package, so the kl line shows the folder keelc would use.
    if( args.count( "print-config" ) )
    {
        fmt::print( "build {}\n", keel::build_id() );
        if( kl != packages.end() )
        {
            fmt::print( "package kl {}\n", kl->root.string() );
        }
        return 0;
    }

    // --runtime is checked before anything is written, so a missing one never reaches cc; without
    // it, the carried runtime is written beside the objects later.
    keel::Runtime runtime;

    if( stop == Stop::Executable && args.count( "runtime" ) )
    {
        runtime = keel::find_runtime( args["runtime"].as<std::string>(), {} );

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

    if( stop == Stop::Imports )
    {
        const std::vector<keel::Token>   tokens = keel::lex( file_id.value(), sm, interner, literals, diagnostics );
        keel::Ast                        ast;
        const std::vector<keel::Node_id> decls = keel::parse_into( ast, tokens, sm, diagnostics );

        if( diagnostics.has_errors() )
        {
            return finish();
        }

        for( const keel::Node_id decl : decls )
        {
            if( ast.kind( decl ) != keel::Node_kind::Import_decl )
            {
                continue;
            }

            const bool has_package = ast.package( decl ).is_valid();
            if( has_package )
            {
                fmt::print( "{}::{}\n", interner.text( ast.name( ast.package( decl ) ) ), interner.text( ast.name( decl ) ) );
            }
            else
            {
                fmt::print( "{}\n", interner.text( ast.name( decl ) ) );
            }
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

    const bool debug           = args.count( "debug" ) > 0;
    const bool overflow_checks = debug || args.count( "overflow-checks" ) > 0;

    // Read and judged before anything is parsed, so a refused object costs nothing.
    std::vector<keel::Object> given;
    for( const std::filesystem::path& path : klos )
    {
        std::string                       error;
        const std::optional<keel::Object> object = keel::read_object( path, error );
        if( !object )
        {
            fmt::print( stderr, "keelc: {}\n", error );
            return 2;
        }
        given.push_back( *object );
    }
    if( const std::optional<std::string> refusal = keel::refused( given, debug, overflow_checks ) )
    {
        fmt::print( stderr, "keelc: {}\n", *refusal );
        return 2;
    }

    // A release object stops where its state is taken: the program that links it optimises it.
    const bool        as_module = stop == Stop::Object;
    const keel::Stage through   = as_module && !debug ? keel::Stage::Reported : keel::Stage::Elaborated;

    // Every other mode needs the whole front end; --check stops after it, leaving no artefacts behind.
    keel::Front_end front = keel::run_front_end(
        file_id.value(),
        through,
        keel::Build {
            .asserts = debug, .overflow_checks = overflow_checks, .optimise = !debug, .keep_checked = as_module && debug
        },
        sm,
        interner,
        literals,
        diagnostics,
        packages,
        as_module,
        given
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

    const keel::Parts parts( ast, types, prog.modules, file_id.value(), functions, front.provided );

    // Additive: the C path below is untouched, and nothing consumes KIR yet. This is what makes
    // each step of the lowerer visible as it lands.
    if( stop == Stop::Kir )
    {
        bool well_formed = true;

        // Every part but the prelude's and the loaded modules'.
        for( const keel::Function& function : functions )
        {
            if( parts.of( function ) == prog.modules.prelude_file() || prog.modules.is_loaded( parts.of( function ) ) )
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

    const auto part_c = [&]( keel::File_id part )
    { return keel::emit_c_from_kir( functions, ast, types, literals, sm, interner, prog.modules, &parts, part ).c; };

    // What each module's object will compile, dependencies first.
    if( stop == Stop::C )
    {
        for( const keel::File_id part : parts.order() )
        {
            if( part != prog.modules.prelude_file() && !prog.modules.is_loaded( part ) )
            {
                std::cout << fmt::format( "// {}\n", sm.file( part ).path ) << part_c( part );
            }
        }

        return finish();
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

    std::filesystem::path output = std::filesystem::path( args["input"].as<std::string>() ).stem();
    if( args.count( "output" ) )
    {
        output = args["output"].as<std::string>();
    }
    else if( stop == Stop::Object )
    {
        output += ".klo";
    }

    // Each part's .c is written beside the output and kept: its #line directives point back at the
    // .kl, so it is the thing to read when the output is wrong. Named after the input, so two
    // programs built side by side do not share one.
    const std::string input_name = std::filesystem::path( args["input"].as<std::string>() ).filename().string();

    if( stop == Stop::Object )
    {
        const keel::Object_options object_options {
            .out      = output,
            .c        = output.parent_path() / ( input_name + ".c" ),
            .debug    = debug,
            .overflow = overflow_checks,
            .compiler = compiler,
            .cflags   = cflags,
            .objects  = given,
        };

        if( const int status = keel::write_object( front, parts, literals, sm, interner, object_options ); status != 0 )
        {
            return status;
        }
        return finish();
    }

    const std::filesystem::path& executable = output;
    const keel::Spelling         spelling { ast, types, interner, prog.modules, sm };

    const auto c_name = [&]( keel::File_id part )
    {
        return part == parts.input() ? input_name + ".c"
               : part == prog.modules.prelude_file()
                   ? input_name + ".prelude.c"
                   : fmt::format( "{}.{}.c", input_name, keel::module_slot( spelling.owner( part ) ) );
    };

    const keel::Temp_dir               object_dir;
    std::vector<std::filesystem::path> sources;
    std::vector<std::filesystem::path> objects;

    if( runtime.path.empty() )
    {
        runtime = keel::find_runtime( std::nullopt, object_dir.path );

        if( !runtime.error.empty() )
        {
            fmt::print( stderr, "keelc: {}\n", runtime.error );
            return 1;
        }
    }

    for( const keel::File_id part : parts.order() )
    {
        // A debug object holds its part compiled; a release one is optimised with the program.
        if( debug && prog.modules.is_loaded( part ) )
        {
            continue;
        }

        const std::filesystem::path path = executable.parent_path() / c_name( part );
        std::ofstream               out( path );

        if( !out )
        {
            fmt::print( stderr, "keelc: cannot write {}\n", path.string() );
            return 1;
        }

        out << part_c( part );
        sources.push_back( path );
        objects.push_back( object_dir.path / std::filesystem::path( c_name( part ) ).replace_extension( ".o" ) );
    }

    // Concurrently, so the wall time is about the largest part's.
    std::vector<std::future<int>> compiles;
    for( std::size_t i = 0; i < sources.size(); ++i )
    {
        compiles.push_back( std::async(
            std::launch::async,
            [command = keel::compile_command( compiler, cflags, sources[i], objects[i] )]
            { return std::system( command.c_str() ); }
        ) );
    }

    bool compiled = true;
    for( std::size_t i = 0; i < compiles.size(); ++i )
    {
        // The program type-checked, so the C it produced should always compile: this is a compiler bug.
        if( compiles[i].get() != 0 )
        {
            fmt::print( stderr, "keelc: the generated C failed to compile - see {}\n", sources[i].string() );
            compiled = false;
        }
    }

    if( !compiled )
    {
        return 1;
    }

    const std::filesystem::path generated_path = sources.back();

    // After the parts, dependents first, so one pass of the linker resolves each into the next.
    if( debug )
    {
        for( auto state = prog.states.rbegin(); state != prog.states.rend(); ++state )
        {
            objects.push_back( state->object );
        }
    }
    const std::string command = keel::link_command( compiler, cflags, objects, runtime.path, executable, linked );

    if( const int status = std::system( command.c_str() ); status != 0 )
    {
        // Without the user's own C, a failure here is a compiler bug: the program type-checked, so
        // the C it produced should always link. The input's path is named so the output can be
        // inspected. With it, the user's C or a missing symbol is the likelier cause.
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
