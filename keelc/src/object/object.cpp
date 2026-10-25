// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "object/object.h"
#include <fmt/core.h>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>
#include "codegen_c/emit_kir.h"
#include "codegen_c/link.h"
#include "codegen_c/mangle.h"
#include "codegen_c/spelling.h"
#include "common/archive.h"
#include "common/bytes.h"
#include "common/temp_dir.h"
#include "common/version.h"
#include "object/manifest.h"
#include "object/state.h"

namespace keel
{

namespace
{

std::optional<std::string> read_file( const std::filesystem::path& path )
{
    std::ifstream in( path, std::ios::binary );
    if( !in )
    {
        return std::nullopt;
    }
    return std::string( std::istreambuf_iterator<char>( in ), {} );
}

bool write_file( const std::filesystem::path& path, std::string_view bytes )
{
    std::ofstream out( path, std::ios::binary );
    out.write( bytes.data(), std::streamsize( bytes.size() ) );
    return bool( out );
}

} // namespace

int write_object(
    Front_end&            front,
    const Parts&          parts,
    const Literal_pool&   literals,
    const Source_manager& sm,
    const Interner&       interner,
    const Object_options& options
)
{
    const Program& program = front.program;
    const File_id  input   = parts.input();

    // The part as the dataflow checks left it: a release build stopped there.
    const std::vector<Function>& checked = options.debug ? front.checked : front.functions;

    std::vector<Function> part;
    for( std::size_t f = 0; f < front.functions.size(); ++f )
    {
        if( parts.of( front.functions[f] ) == input )
        {
            part.push_back( checked[f] );
        }
    }

    const Manifest manifest {
        .build       = std::string( build_id() ),
        .debug       = options.debug,
        .overflow    = options.overflow,
        .module      = module_name( program.modules, sm, interner, input ),
        .source      = sm.file( input ).path,
        .source_hash = fnv1a( sm.file( input ).text ),
    };

    std::vector<Archive_member> members;
    members.push_back( { .name = "__.KEEL", .data = write_manifest( manifest ) } );
    members.push_back(
        { .name = "__.KEEL.STATE",
          .data = write_state( program, front.resolution, front.types, part, input, literals, sm, interner ) }
    );

    // A release build compiles each part in the program that links it, optimised across modules.
    if( options.debug )
    {
        const Emitted emitted = emit_c_from_kir(
            front.functions, program.ast, front.types, literals, sm, interner, program.modules, &parts, input, true
        );

        if( !write_file( options.c, emitted.c ) )
        {
            fmt::print( stderr, "keelc: cannot write {}\n", options.c.string() );
            return 1;
        }

        const Temp_dir              object_dir;
        const std::filesystem::path native = object_dir.path / "part.o";

        // The program type-checked, so the C it produced should always compile: this is a compiler bug.
        const std::string                command = compile_command( options.compiler, options.cflags, options.c, native );
        const std::optional<std::string> bytes   = std::system( command.c_str() ) == 0 ? read_file( native ) : std::nullopt;
        if( !bytes )
        {
            fmt::print( stderr, "keelc: the generated C failed to compile - see {}\n", options.c.string() );
            return 1;
        }

        const Spelling spelling { program.ast, front.types, interner, program.modules, sm };
        members.push_back( { .name = module_slot( spelling.owner( input ) ) + ".o", .data = *bytes, .symbols = emitted.defined }
        );
    }

    // Renamed into place, so an interrupted build never leaves half an object.
    std::filesystem::path temporary = options.out;
    temporary += ".tmp";
    bool written = write_file( temporary, write_archive( members ) );
    if( written )
    {
        std::error_code renamed;
        std::filesystem::rename( temporary, options.out, renamed );
        written = !renamed;
    }

    if( !written )
    {
        fmt::print( stderr, "keelc: cannot write {}\n", options.out.string() );
        return 1;
    }

    return 0;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <sstream>

namespace keel
{
namespace
{

constexpr std::string_view k_clock = R"(struct tick<T> where T : copyable
{
    T at;
};

i32 ticks = 0;

private i32 step( i32 by ) { return by + 1; }

T read<T>( tick<T> t ) where T : copyable
{
    return t.at;
}

i32 advance( i32 by )
{
    ticks = ticks + step( by );
    return read( tick { ticks } );
}
)";

struct Built
{
    std::string                 bytes;
    std::vector<Archive_member> members;
    std::vector<std::string>    advance_and_read; // the spellings write_object's index should hold
    std::string                 rendered;
};

// `kl::clock` compiled as `-c kl/clock.kl --package kl=kl` would.
Built build_clock( const Temp_dir& dir, bool debug )
{
    const std::filesystem::path path = dir.write( "kl/clock.kl", k_clock );
    const Package               kl { "kl", dir.path / "kl" };

    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;
    const File_id  input = *sm.load_file( path );

    Front_end front = run_front_end(
        input,
        debug ? Stage::Elaborated : Stage::Reported,
        { .asserts = debug, .overflow_checks = debug, .optimise = !debug, .keep_checked = debug },
        sm,
        interner,
        literals,
        diags,
        std::span( &kl, 1 ),
        true
    );

    Built              built;
    std::ostringstream rendered;
    diags.render( sm, rendered );
    built.rendered = rendered.str();
    if( diags.has_errors() )
    {
        return built;
    }

    const Parts                 parts( front.program.ast, front.types, front.program.modules, input, front.functions );
    const std::filesystem::path out = dir.path / "clock.klo";
    REQUIRE(
        write_object(
            front,
            parts,
            literals,
            sm,
            interner,
            { .out = out, .c = dir.path / "clock.kl.c", .debug = debug, .overflow = debug }
        ) == 0
    );

    const Spelling spelling { front.program.ast, front.types, interner, front.program.modules, sm };
    for( const Function& function : front.functions )
    {
        const std::string name = spelling.function( function.declaration, function.type_arguments );
        if( parts.of( function ) == input &&
            ( name.find( "advance" ) != std::string::npos || name.find( "read" ) != std::string::npos ) )
        {
            built.advance_and_read.push_back( name );
        }
    }

    built.bytes   = *read_file( out );
    built.members = *read_archive( built.bytes );
    return built;
}

} // namespace

TEST_CASE( "write_object_writes_the_manifest_the_state_and_the_native_part", "[object]" )
{
    const Temp_dir dir;
    const Built    built = build_clock( dir, true );
    INFO( built.rendered );
    REQUIRE( built.rendered.empty() );

    REQUIRE( built.members.size() == 3 );
    REQUIRE( built.members[0].name == "__.KEEL" );
    REQUIRE( built.members[1].name == "__.KEEL.STATE" );
    REQUIRE( built.members[2].name == "2kl5clock.o" );

    const std::optional<Manifest> manifest = read_manifest( built.members[0].data );
    REQUIRE( manifest.has_value() );
    REQUIRE( manifest->module == "kl::clock" );
    REQUIRE( manifest->build == build_id() );
    REQUIRE( manifest->debug );
    REQUIRE( manifest->depends.empty() );

    const std::optional<Source_header> source = read_source( built.members[1].data );
    REQUIRE( source.has_value() );
    REQUIRE( source->text == k_clock );

    // read_archive skips the index, so the symbols are checked where the .o defines them.
    REQUIRE( built.advance_and_read.size() == 2 );
    for( const std::string& name : built.advance_and_read )
    {
        INFO( name );
        REQUIRE( built.members[2].data.find( name ) != std::string::npos );
        REQUIRE( built.bytes.substr( 0, built.bytes.find( "__.KEEL" ) ).find( name ) != std::string::npos ); // in the index
    }
}

TEST_CASE( "write_object_writes_the_same_bytes_twice", "[object]" )
{
    const Temp_dir dir;
    const Built    first  = build_clock( dir, true );
    const Built    second = build_clock( dir, true );
    REQUIRE( first.rendered.empty() );
    REQUIRE( first.bytes == second.bytes );
}

TEST_CASE( "write_object_in_release_writes_no_native_part", "[object]" )
{
    const Temp_dir dir;
    const Built    built = build_clock( dir, false );
    INFO( built.rendered );
    REQUIRE( built.rendered.empty() );

    REQUIRE( built.members.size() == 2 );
    REQUIRE( !read_manifest( built.members[0].data )->debug );
}

} // namespace keel
#endif
