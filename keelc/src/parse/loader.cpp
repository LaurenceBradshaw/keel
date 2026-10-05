// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parse/loader.h"
#include <fmt/format.h>
#include <unordered_set>
#include <vector>
#include "common/imports.h"
#include "common/types.h"
#include "lex/lexer.h"
#include "parse/parser.h"

namespace keel
{
Program load_program(
    File_id                  input,
    Source_manager&          sm,
    Interner&                interner,
    Literal_pool&            literals,
    Diagnostics&             diags,
    std::span<const Package> packages,
    std::string_view         prelude
)
{
    const File_id prelude_file = sm.add_file( std::string( prelude_path ), std::string( prelude ) );

    // A stack: the prelude is parsed last, so the program's node ids do not move as it grows.
    std::vector<File_id>    pending { prelude_file, input };
    std::unordered_set<u32> loaded;
    loaded.insert( prelude_file.v );
    loaded.insert( input.v );

    const std::filesystem::path program_folder = std::filesystem::path( sm.file( input ).path ).parent_path();

    Ast     ast;
    Imports imports;
    imports.set_prelude( prelude_file, interner.intern( "prelude" ) );

    std::unordered_map<u32, std::filesystem::path> package_roots;
    for( const Package& package : packages )
    {
        const Symbol_id package_id = interner.intern( package.name );
        imports.add_package( package_id );

        package_roots[package_id.v] = package.root;
    }

    std::vector<Node_id> decls;

    while( !pending.empty() )
    {
        const File_id current = pending.back();
        pending.pop_back();

        const std::vector<Token>   tokens     = lex( current, sm, interner, literals, diags );
        const std::vector<Node_id> file_decls = parse_into( ast, tokens, sm, diags );

        for( const Node_id decl : file_decls )
        {
            if( ast.kind( decl ) == Node_kind::Import_decl )
            {
                Symbol_id package_id = !ast.children( decl ).empty() ? Symbol_id { ast.aux( ast.child( decl, 0 ) ) }
                                                                     : imports.package_of( current );

                std::filesystem::path module_folder = program_folder;
                if( package_id.is_valid() )
                {
                    if( const auto it = package_roots.find( package_id.v ); it != package_roots.end() )
                    {
                        module_folder = it->second;
                    }
                    else
                    {
                        if( !imports.is_missing( package_id ) )
                        {
                            diags.error(
                                ast.span( decl ),
                                fmt::format( "there is no package `{}`", interner.text( package_id ) ),
                                fmt::format( "a package is named with `--package {}=<dir>`", interner.text( package_id ) )
                            );
                        }
                        imports.add_missing_package( package_id );
                        continue;
                    }
                }

                const std::string_view      module_name = interner.text( Symbol_id { ast.aux( decl ) } );
                const std::filesystem::path module_path = module_folder / ( std::string( module_name ) + ".kl" );

                // Dedupes by canonical path, which is what stops cycles and diamonds
                const std::optional<File_id> module = sm.load_file( module_path );

                if( !module.has_value() )
                {
                    diags.error(
                        ast.span( decl ),
                        fmt::format( "there is no module `{}`", qualified( interner, package_id, module_name ) ),
                        package_id.is_valid()
                            ? fmt::format( "it would be `{}.kl` in the package `{}`", module_name, interner.text( package_id ) )
                            : fmt::format(
                                  "`import {};` looks for `{}.kl` beside the file being compiled", module_name, module_name
                              )
                    );
                    continue;
                }

                imports.add( current, *module );
                if( loaded.insert( module->v ).second )
                {
                    imports.place( *module, package_id );
                    pending.push_back( *module );
                }
            }
            else
            {
                decls.push_back( decl );
            }
        }
    }

    const Node_id root =
        ast.add( Node_kind::Source_file, Span { input, 0, narrow_cast<u32>( sm.file( input ).text.size() ) }, 0, decls );
    ast.set_root( root );

    Program prog;
    prog.ast     = std::move( ast );
    prog.imports = std::move( imports );
    return prog;
}
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/temp_dir.h"
#include "loader.h"

namespace keel
{
namespace
{

// A program on disk: `main.kl` and whatever else it names, loaded the way the driver loads one. The
// package `kl` is the directory `kl/` beside it.
class Loaded
{
public:
    explicit Loaded( std::initializer_list<std::pair<const char*, std::string_view>> files, std::string_view prelude = {} )
    {
        for( const auto& [name, text] : files )
        {
            dir_.write( name, text );
        }

        const std::optional<File_id> input = sm_.load_file( dir_.path / "main.kl" );

        REQUIRE( input.has_value() );
        const Package kl { .name = "kl", .root = dir_.path / "kl" };
        program_ = load_program( *input, sm_, interner_, literals_, diags_, std::span( &kl, 1 ), prelude );

        for( const auto& [name, text] : files )
        {
            files_[name] = *sm_.load_file( dir_.path / name ); // already loaded, so the same id
        }

        for( u32 i = 0; i < sm_.file_count(); ++i )
        {
            if( sm_.file( File_id { i } ).path == prelude_path )
            {
                files_[std::string( prelude_path )] = File_id { i };
            }
        }
    }

    const Ast& ast() const
    {
        return program_.ast;
    }

    // Whether the file `from` may name what `to` declares.
    bool sees( std::string_view from, std::string_view to ) const
    {
        return program_.imports.sees( files_.at( std::string( from ) ), files_.at( std::string( to ) ) );
    }

    // The package `file` was loaded from; empty for the program's own.
    std::string_view package_of( std::string_view file ) const
    {
        const Symbol_id package = program_.imports.package_of( files_.at( std::string( file ) ) );
        return package.is_valid() ? interner_.text( package ) : std::string_view {};
    }

    bool is_package( std::string_view name )
    {
        return program_.imports.is_package( interner_.intern( name ) );
    }

    std::size_t errors() const
    {
        return diags_.error_count();
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags_.render( sm_, out );
        return out.str();
    }

    // The path of the file the tree's last declaration is in.
    std::string_view last_file() const
    {
        return sm_.file( ast().span( ast().children( ast().root() ).back() ).file ).path;
    }

    // Every top-level function's name, sorted, so a test can ask which were loaded and how often.
    std::vector<std::string_view> functions() const
    {
        std::vector<std::string_view> names;

        for( const Node_id decl : ast().children( ast().root() ) )
        {
            if( ast().kind( decl ) == Node_kind::Function_decl )
            {
                names.push_back( interner_.text( Symbol_id { ast().aux( decl ) } ) );
            }
        }

        std::ranges::sort( names );
        return names;
    }

private:
    Temp_dir       dir_;
    Source_manager sm_;
    Interner       interner_;
    Literal_pool   literals_;
    Diagnostics    diags_;
    Program        program_;

    std::map<std::string, File_id> files_;
};

} // namespace

TEST_CASE( "loader_loads_a_program_from_its_imports", "[parse][loader]" )
{
    SECTION( "an import brings the module's declarations in" )
    {
        const Loaded p( {
            { "main.kl", "import shape;\ni32 main() { return area(); }\n" },
            { "shape.kl", "i32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );

        const std::span<const Node_id> decls = p.ast().children( p.ast().root() );

        REQUIRE( decls.size() == 2 );
        REQUIRE( std::ranges::none_of( decls, [&]( Node_id d ) { return p.ast().kind( d ) == Node_kind::Import_decl; } ) );
        REQUIRE( p.ast().span( decls[0] ).file != p.ast().span( decls[1] ).file );
    }

    SECTION( "imports are transitive" )
    {
        const Loaded p( {
            { "main.kl", "import a;\ni32 main() { return fa(); }\n" },
            { "a.kl", "import b;\ni32 fa() { return fb(); }\n" },
            { "b.kl", "i32 fb() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "fa", "fb", "main" } );
    }

    // Loading follows every import; seeing follows only the file's own.
    SECTION( "a file sees itself and what it imports, and no further" )
    {
        const Loaded p( {
            { "main.kl", "import a;\ni32 main() { return fa(); }\n" },
            { "a.kl", "import b;\nimport main;\ni32 fa() { return fb(); }\n" },
            { "b.kl", "i32 fb() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );

        REQUIRE( p.sees( "main.kl", "main.kl" ) );
        REQUIRE( p.sees( "main.kl", "a.kl" ) );
        REQUIRE_FALSE( p.sees( "main.kl", "b.kl" ) );

        REQUIRE( p.sees( "a.kl", "b.kl" ) );
        REQUIRE( p.sees( "a.kl", "main.kl" ) );

        REQUIRE( p.sees( "b.kl", "b.kl" ) );
        REQUIRE_FALSE( p.sees( "b.kl", "a.kl" ) );
    }

    // A diamond reaches `c` twice, and `c` importing `main` closes a cycle. Each file is read once.
    SECTION( "a module is loaded once" )
    {
        const Loaded p( {
            { "main.kl", "import a;\nimport b;\ni32 main() { return fa() + fb(); }\n" },
            { "a.kl", "import c;\ni32 fa() { return fc(); }\n" },
            { "b.kl", "import c;\ni32 fb() { return fc(); }\n" },
            { "c.kl", "import main;\ni32 fc() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "fa", "fb", "fc", "main" } );
    }

    SECTION( "a missing module is reported at its import" )
    {
        const Loaded p( {
            { "main.kl", "import nope;\ni32 main() { return 0; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "there is no module `nope`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`nope.kl`" ) != std::string::npos );
    }

    SECTION( "an import goes first" )
    {
        const Loaded p( {
            { "main.kl", "i32 f() { return 1; }\nimport shape;\ni32 main() { return f(); }\n" },
            { "shape.kl", "i32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "an `import` goes before every declaration" ) != std::string::npos );
    }
}

TEST_CASE( "loader_loads_modules_from_a_package", "[parse][loader][packages]" )
{
    // The program's own `geom.kl` is not what `kl::geom` names, and is not loaded.
    SECTION( "a package's module is loaded from the package's directory" )
    {
        const Loaded p( {
            { "main.kl", "import kl::geom;\ni32 main() { return 0; }\n" },
            { "geom.kl", "i32 decoy() { return 1; }\n" },
            { "kl/geom.kl", "i32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "area", "main" } );
        REQUIRE( p.sees( "main.kl", "kl/geom.kl" ) );
        REQUIRE( p.package_of( "kl/geom.kl" ) == "kl" );
        REQUIRE( p.package_of( "main.kl" ).empty() );
    }

    SECTION( "a bare import inside a package looks in the package" )
    {
        const Loaded p( {
            { "main.kl", "import kl::geom;\ni32 main() { return 0; }\n" },
            { "shape.kl", "i32 decoy() { return 1; }\n" },
            { "kl/geom.kl", "import shape;\ni32 area() { return side(); }\n" },
            { "kl/shape.kl", "i32 side() { return 2; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "area", "main", "side" } );
        REQUIRE( p.package_of( "kl/shape.kl" ) == "kl" );
        REQUIRE( p.sees( "kl/geom.kl", "kl/shape.kl" ) );
    }

    // Inside `kl` the qualifier is optional, so both spellings reach the same file.
    SECTION( "a package may name itself" )
    {
        const Loaded p( {
            { "main.kl", "import kl::geom;\ni32 main() { return 0; }\n" },
            { "kl/geom.kl", "import kl::shape;\ni32 area() { return side(); }\n" },
            { "kl/shape.kl", "i32 side() { return 2; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "area", "main", "side" } );
    }

    SECTION( "a module of the program and one of a package may share a name" )
    {
        const Loaded p( {
            { "main.kl", "import geom;\nimport kl::geom;\ni32 main() { return 0; }\n" },
            { "geom.kl", "i32 own_area() { return 1; }\n" },
            { "kl/geom.kl", "i32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "area", "main", "own_area" } );
        REQUIRE( p.package_of( "geom.kl" ).empty() );
    }

    // Named on the command line, so known before anything is imported from it.
    SECTION( "a package is known without an import" )
    {
        Loaded p( {
            { "main.kl", "i32 main() { return 0; }\n" },
        } );

        REQUIRE( p.is_package( "kl" ) );
        REQUIRE_FALSE( p.is_package( "main" ) );
        REQUIRE_FALSE( p.is_package( "foo" ) );
    }

    SECTION( "an unknown package is reported at its import" )
    {
        const Loaded p( {
            { "main.kl", "import foo::geom;\ni32 main() { return 0; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "there is no package `foo`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "main.kl:1:1" ) != std::string::npos );
    }

    SECTION( "a missing package is reported once, however many imports name it" )
    {
        const Loaded p( {
            { "main.kl", "import foo::geom;\nimport foo::shapes;\ni32 main() { return 0; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "main.kl:1:1" ) != std::string::npos );
    }

    SECTION( "a missing module of a package is reported at its import" )
    {
        const Loaded p( {
            { "main.kl", "import kl::nope;\ni32 main() { return 0; }\n" },
            { "nope.kl", "i32 decoy() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "there is no module `kl::nope`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`nope.kl`" ) != std::string::npos );
    }
}

TEST_CASE( "loader_loads_the_prelude_with_every_program", "[parse][loader][prelude]" )
{
    SECTION( "every file sees it without an import" )
    {
        const Loaded p(
            {
                { "main.kl", "import a;\ni32 main() { return answer() + fa(); }\n" },
                { "a.kl", "i32 fa() { return answer(); }\n" },
            },
            "i32 answer() { return 42; }\n"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "answer", "fa", "main" } );
        REQUIRE( p.sees( "main.kl", "<prelude>" ) );
        REQUIRE( p.sees( "a.kl", "<prelude>" ) );
        REQUIRE_FALSE( p.sees( "<prelude>", "main.kl" ) );
    }

    SECTION( "its declarations follow the program's" )
    {
        const Loaded p( { { "main.kl", "i32 main() { return answer(); }\n" } }, "i32 answer() { return 42; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.last_file() == "<prelude>" );
    }

    // So it never shares a scope with the program's own declarations, and a qualifier cannot name it.
    SECTION( "it is in a package of its own that is not a package by name" )
    {
        Loaded p( { { "main.kl", "i32 main() { return 0; }\n" } }, "i32 answer() { return 42; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.package_of( "<prelude>" ) == "prelude" );
        REQUIRE_FALSE( p.is_package( "prelude" ) );
    }

    SECTION( "an error in it is reported at its own path" )
    {
        const Loaded p( { { "main.kl", "i32 main() { return 0; }\n" } }, "i32 answer( { return 42; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
        REQUIRE( p.rendered().find( "<prelude>:1:" ) != std::string::npos );
    }

    SECTION( "the shipped prelude parses cleanly" )
    {
        const Loaded p( { { "main.kl", "i32 main() { return 0; }\n" } }, prelude_source() );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
