// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "object/object.h"
#include <fmt/core.h>
#include <algorithm>
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

// Each direct import of `input`, by its object's hash, sorted by module.
std::vector<std::pair<std::string, u64>> depends(
    const Program& program, const Source_manager& sm, const Interner& interner, File_id input, std::span<const Object> objects
)
{
    std::vector<std::pair<std::string, u64>> depends;
    for( const Loaded_state& state : program.states )
    {
        if( !program.modules.sees( input, state.file ) )
        {
            continue;
        }

        const std::string module = module_name( program.modules, sm, interner, state.file );
        const auto        object = std::ranges::find( objects, module, &Object::module );
        depends.emplace_back( module, object->hash );
    }

    std::ranges::sort( depends );
    return depends;
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
        .depends     = depends( program, sm, interner, input, options.objects ),
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

std::optional<Object> read_object( const std::filesystem::path& path, std::string& error )
{
    const std::optional<std::string> bytes = read_file( path );
    if( !bytes )
    {
        error = fmt::format( "cannot open '{}'", path.string() );
        return std::nullopt;
    }

    Object object { .path = path.string(), .hash = fnv1a( *bytes ) };

    const std::optional<std::vector<Archive_member>> members = read_archive( *bytes );
    const auto                                       member  = [&]( std::string_view name ) -> const Archive_member*
    {
        const auto it = std::ranges::find( *members, name, &Archive_member::name );
        return it != members->end() ? &*it : nullptr;
    };

    const Archive_member*         manifest = members ? member( "__.KEEL" ) : nullptr;
    const Archive_member*         state    = members ? member( "__.KEEL.STATE" ) : nullptr;
    const std::optional<Manifest> read     = manifest ? read_manifest( manifest->data ) : std::nullopt;
    if( !read || !state )
    {
        error = fmt::format( "'{}' is not a Keel object", path.string() );
        return std::nullopt;
    }

    object.manifest = *read;
    object.state    = state->data;
    return object;
}

std::optional<std::string> refused( std::span<const Object> objects, bool debug, bool overflow )
{
    const auto named = [&]( std::string_view module ) -> const Object*
    {
        for( const Object& object : objects )
        {
            if( object.manifest.module == module )
            {
                return &object;
            }
        }
        return nullptr;
    };

    for( const Object& object : objects )
    {
        const Manifest& manifest = object.manifest;

        if( manifest.build != build_id() )
        {
            return fmt::format( "'{}' was built by another keelc; build it again", object.path );
        }

        if( manifest.debug != debug )
        {
            return fmt::format(
                "'{}' is a {} object, and this is a {} build",
                object.path,
                manifest.debug ? "debug" : "release",
                debug ? "debug" : "release"
            );
        }

        if( manifest.overflow != overflow )
        {
            return fmt::format(
                "'{}' was built {} overflow checks, and this build {} them",
                object.path,
                manifest.overflow ? "with" : "without",
                overflow ? "has" : "does not have"
            );
        }

        if( const Object* first = named( manifest.module ); first != &object )
        {
            return fmt::format( "'{}' and '{}' are both `{}`", first->path, object.path, manifest.module );
        }

        for( const auto& [module, hash] : manifest.depends )
        {
            const Object* dependency = named( module );
            if( dependency == nullptr )
            {
                return fmt::format(
                    "'{}' was built against `{}`, and no object for `{}` was given", object.path, module, module
                );
            }

            if( dependency->hash != hash )
            {
                return fmt::format(
                    "'{}' was built against another `{}` than '{}'; build '{}' again",
                    object.path,
                    module,
                    dependency->path,
                    object.path
                );
            }
        }
    }

    return std::nullopt;
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

namespace
{

constexpr std::string_view k_counter = R"(struct box_of<T> where T : copyable
{
    T value;
};

box_of<T> pack<T>( T value ) where T : copyable
{
    return box_of { value };
}

T open_box<T>( box_of<T> b ) where T : copyable
{
    return b.value;
}

i32 count = 0;

i32 bump( i32 by )
{
    count = count + by;
    return open_box( pack( count ) );
}
)";

// A new instance of each of `counter`'s generics, and one `counter` made itself.
constexpr std::string_view k_counter_main = R"(import kl::counter;

i32 main()
{
    bool yes = kl::open_box( kl::pack( true ) );
    if( !yes )
    {
        return 1;
    }
    return kl::bump( 2 ) - 2;
}
)";

constexpr std::string_view k_user = R"(import counter;

bool flip( bool b )
{
    return !open_box( pack( b ) );
}
)";

// `relative`, under `kl`, compiled as `-c` in release would, its imports given as `objects`.
std::filesystem::path compile_object( const Temp_dir& dir, const std::string& relative, std::span<const Object> objects )
{
    const Package  kl { "kl", dir.path / "kl" };
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;
    const File_id  input = *sm.load_file( dir.path / relative );

    Front_end front =
        run_front_end( input, Stage::Reported, {}, sm, interner, literals, diags, std::span( &kl, 1 ), true, objects );

    std::ostringstream rendered;
    diags.render( sm, rendered );
    INFO( rendered.str() );
    REQUIRE( !diags.has_errors() );

    const Parts                 parts( front.program.ast, front.types, front.program.modules, input, front.functions );
    const std::filesystem::path out = dir.path / ( std::filesystem::path( relative ).stem().string() + ".klo" );
    REQUIRE( write_object( front, parts, literals, sm, interner, { .out = out, .objects = objects } ) == 0 );
    return out;
}

// A program checked, its imports given as source under `packages` or as `objects`.
struct Checked
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;
    File_id        input {};
    Program        program;
    Resolution     resolution;
    Types          types;

    Checked( const std::filesystem::path& main, std::span<const Package> packages, std::span<const Object> objects )
    {
        input      = *sm.load_file( main );
        program    = load_program( input, sm, interner, literals, diags, packages, false, objects );
        resolution = resolve( program.ast, sm, interner, diags, program.modules, program.states );
        types =
            type_check( program.ast, resolution, literals, sm, interner, diags, std::move( program.types ), program.states );
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags.render( sm, out );
        return out.str();
    }

    std::string name( Node_id decl ) const
    {
        return decl.is_valid() ? std::string( interner.text( program.ast.name( decl ) ) ) : "";
    }

    // Each instantiation, as `name<arguments>`, sorted.
    std::vector<std::string> instantiations() const
    {
        std::vector<std::string> named;
        for( const Instantiation& instance : types.instantiations() )
        {
            std::string text = name( instance.declaration ) + "<";
            for( const Type_id argument : instance.arguments )
            {
                text += std::string( types.table().name( argument ) ) + ",";
            }
            named.push_back( text + ">" );
        }
        std::ranges::sort( named );
        return named;
    }
};

Object object_named( std::string path, std::string module, u64 hash, std::vector<std::pair<std::string, u64>> depends = {} )
{
    return Object {
        .path = std::move( path ),
        .hash = hash,
        .manifest =
            { .build    = std::string( build_id() ),
              .debug    = true,
              .overflow = true,
              .module   = std::move( module ),
              .depends  = std::move( depends ) },
    };
}

} // namespace

TEST_CASE( "a_module_given_as_an_object_checks_as_its_source_does", "[object]" )
{
    const Temp_dir dir;
    dir.write( "kl/counter.kl", k_counter );
    const std::filesystem::path main = dir.write( "main.kl", k_counter_main );
    const Package               kl { "kl", dir.path / "kl" };

    std::string                 error;
    const std::optional<Object> counter = read_object( compile_object( dir, "kl/counter.kl", {} ), error );
    INFO( error );
    REQUIRE( counter );
    REQUIRE( counter->module() == "kl::counter" );

    const Checked from_source( main, std::span( &kl, 1 ), {} );
    const Checked from_object( main, {}, std::span( &*counter, 1 ) );
    INFO( from_object.rendered() );
    REQUIRE( from_source.rendered().empty() );
    REQUIRE( from_object.rendered().empty() );

    // `main.kl` is parsed first in both, so its nodes have the same ids.
    const Ast& a       = from_source.program.ast;
    const Ast& b       = from_object.program.ast;
    u32        checked = 0;
    for( Node_id id { 0 }; a.span( id ).file == from_source.input; ++id.v, ++checked )
    {
        INFO( node_kind_name( a.kind( id ) ) << " at " << a.span( id ).start );
        REQUIRE( b.span( id ).file == from_object.input );
        REQUIRE( a.kind( id ) == b.kind( id ) );

        const Type_id type_a = from_source.types.type_of( id );
        const Type_id type_b = from_object.types.type_of( id );
        REQUIRE( type_a.is_valid() == type_b.is_valid() );
        if( type_a.is_valid() )
        {
            REQUIRE( from_source.types.table().name( type_a ) == from_object.types.table().name( type_b ) );
        }

        REQUIRE(
            from_source.name( from_source.resolution.declaration_of( id ) ) ==
            from_object.name( from_object.resolution.declaration_of( id ) )
        );
        REQUIRE(
            from_source.name( from_source.types.callee_of( id ) ) == from_object.name( from_object.types.callee_of( id ) )
        );
    }
    REQUIRE( checked > 20 );

    REQUIRE( from_object.instantiations() == from_source.instantiations() );
    REQUIRE( from_object.instantiations().size() == 4 ); // pack and open_box, at bool and i32
}

TEST_CASE( "an_import_under_c_is_given_as_an_object", "[object]" )
{
    const Temp_dir dir;
    dir.write( "kl/counter.kl", k_counter );
    const std::filesystem::path user = dir.write( "kl/user.kl", k_user );
    const Package               kl { "kl", dir.path / "kl" };

    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;
    load_program( *sm.load_file( user ), sm, interner, literals, diags, std::span( &kl, 1 ), true );
    std::ostringstream rendered;
    diags.render( sm, rendered );
    REQUIRE( rendered.str().find( "`kl::counter` is imported, but no object for it was given" ) != std::string::npos );

    std::string                 error;
    const std::optional<Object> counter = read_object( compile_object( dir, "kl/counter.kl", {} ), error );
    REQUIRE( counter );

    const std::optional<Object> built = read_object( compile_object( dir, "kl/user.kl", std::span( &*counter, 1 ) ), error );
    REQUIRE( built );
    REQUIRE( built->manifest.depends == std::vector<std::pair<std::string, u64>> { { "kl::counter", counter->hash } } );
}

TEST_CASE( "read_object_refuses_what_is_not_an_object", "[object]" )
{
    const Temp_dir dir;
    std::string    error;

    REQUIRE( !read_object( dir.path / "absent.klo", error ) );
    REQUIRE( error == fmt::format( "cannot open '{}'", ( dir.path / "absent.klo" ).string() ) );

    const std::filesystem::path text = dir.write( "text.klo", "hello\n" );
    REQUIRE( !read_object( text, error ) );
    REQUIRE( error == fmt::format( "'{}' is not a Keel object", text.string() ) );
}

TEST_CASE( "refused_names_the_first_object_that_cannot_be_built_with", "[object]" )
{
    const Object optional = object_named( "optional.klo", "kl::optional", 1 );
    const Object list     = object_named( "list.klo", "kl::list", 2, { { "kl::optional", 1 } } );

    REQUIRE( !refused( std::vector { optional, list }, true, true ) );

    REQUIRE(
        refused( std::vector { optional, list }, false, true ) ==
        "'optional.klo' is a debug object, and this is a release build"
    );
    REQUIRE(
        refused( std::vector { optional, list }, true, false ) ==
        "'optional.klo' was built with overflow checks, and this build does not have them"
    );

    Object other = optional;
    other.manifest.build += "0";
    REQUIRE( refused( std::vector { other }, true, true ) == "'optional.klo' was built by another keelc; build it again" );

    REQUIRE(
        refused( std::vector { optional, object_named( "optional2.klo", "kl::optional", 3 ) }, true, true ) ==
        "'optional.klo' and 'optional2.klo' are both `kl::optional`"
    );
    REQUIRE(
        refused( std::vector { list }, true, true ) ==
        "'list.klo' was built against `kl::optional`, and no object for `kl::optional` was given"
    );
    REQUIRE(
        refused( std::vector { object_named( "optional.klo", "kl::optional", 9 ), list }, true, true ) ==
        "'list.klo' was built against another `kl::optional` than 'optional.klo'; build 'list.klo' again"
    );
}

} // namespace keel
#endif
