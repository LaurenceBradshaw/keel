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
Program load_program( File_id input, Source_manager& sm, Interner& interner, Literal_pool& literals, Diagnostics& diags )
{
    std::vector<File_id>    pending { input };
    std::unordered_set<u32> loaded;
    loaded.insert( input.v );

    Ast                  ast;
    Imports              imports;
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
                const std::filesystem::path dir         = std::filesystem::path( sm.file( input ).path ).parent_path();
                const std::string_view      module_name = interner.text( Symbol_id { ast.aux( decl ) } );
                const std::filesystem::path module_path = dir / ( std::string( module_name ) + ".kl" );

                // Dedupes by canonical path, which is what stops cycles and diamonds
                const std::optional<File_id> module = sm.load_file( module_path );

                if( !module.has_value() )
                {
                    diags.error(
                        ast.span( decl ),
                        fmt::format( "there is no module `{}`", module_name ),
                        fmt::format( "`import {};` looks for `{}.kl` beside the file being compiled", module_name, module_name )
                    );
                    continue;
                }

                imports.add( current, *module );
                if( loaded.insert( module->v ).second )
                {
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

// A program on disk: `main.kl` and whatever else it names, loaded the way the driver loads one.
class Loaded
{
public:
    explicit Loaded( std::initializer_list<std::pair<const char*, std::string_view>> files )
    {
        for( const auto& [name, text] : files )
        {
            dir_.write( name, text );
        }

        const std::optional<File_id> input = sm_.load_file( dir_.path / "main.kl" );

        REQUIRE( input.has_value() );
        program_ = load_program( *input, sm_, interner_, literals_, diags_ );

        for( const auto& [name, text] : files )
        {
            files_[name] = *sm_.load_file( dir_.path / name ); // already loaded, so the same id
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

    SECTION( "a package path is refused for now" )
    {
        const Loaded p( {
            { "main.kl", "import kl::list;\ni32 main() { return 0; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "only a module of this program can be imported" ) != std::string::npos );
    }
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
