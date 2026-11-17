// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parse/loader.h"
#include <fmt/format.h>
#include <algorithm>
#include <unordered_set>
#include <vector>
#include "common/types.h"
#include "lex/lexer.h"
#include "parse/module_graph.h"
#include "parse/parser.h"

namespace keel
{
namespace
{

// What `packageinfo.kl` declares beside its `//!` comments: `@package( name );`, or nothing.
struct Package_header
{
    std::optional<Span> name;  // the declared name
    std::optional<Span> wrong; // the first token that is not part of it
};

Package_header scan_package_header( std::span<const Token> tokens, const Source_manager& sm )
{
    const auto is = [&]( std::size_t i, Token_kind kind ) { return i < tokens.size() && tokens[i].is( kind ); };

    Package_header header;
    std::size_t    next = 0;
    if( is( 0, Token_kind::At ) && is( 1, Token_kind::Identifier ) && sm.text( tokens[1].span ) == "package" )
    {
        if( is( 2, Token_kind::L_paren ) && is( 3, Token_kind::Identifier ) && is( 4, Token_kind::R_paren ) &&
            is( 5, Token_kind::Semicolon ) )
        {
            header.name = tokens[3].span;
            next        = 6;
        }
        else
        {
            header.wrong = Span::merge( tokens[0].span, tokens[1].span );
            return header;
        }
    }
    if( !is( next, Token_kind::End_of_file ) )
    {
        header.wrong = tokens[next].span;
    }
    return header;
}

struct Package_info
{
    std::string                doc;
    std::optional<std::string> name; // as `@package` declares it
};

// A package's `//!` documentation and declared name, from `packageinfo.kl` in its folder. Lexed and
// never parsed, so nothing written in it is declared or imported. `nested` is the name of a nested
// package, whose name is where it is.
Package_info read_package_info(
    const std::filesystem::path& folder,
    Source_manager&              sm,
    Interner&                    interner,
    Literal_pool&                literals,
    Diagnostics&                 diags,
    std::string_view             nested = {}
)
{
    const std::optional<File_id> file = sm.load_file( folder / "packageinfo.kl" );

    if( !file.has_value() )
    {
        return {};
    }

    const std::vector<Token> tokens = lex( *file, sm, interner, literals, diags );
    const Package_header     header = scan_package_header( tokens, sm );

    if( header.wrong )
    {
        const bool directive = tokens.front().is( Token_kind::At );
        diags.error(
            *header.wrong,
            directive ? "`@package` takes the package's name, `@package( name );`"
                      : "`packageinfo.kl` holds only comments and `@package( name );`",
            directive ? "a package's name is an identifier" : "it is the package's documentation; move this into a module"
        );
        return {};
    }

    Package_info info { doc_comment( sm.file( *file ).text, "//!" ), std::nullopt };
    if( header.name )
    {
        const std::string name( sm.text( *header.name ) );
        if( !nested.empty() )
        {
            diags.error(
                *header.name,
                "a nested package's name is its folder's",
                fmt::format( "remove `@package`; this package is `{}`", nested )
            );
        }
        else if( const std::optional<std::string> error = package_name_error( name, interner ) )
        {
            diags.error( *header.name, fmt::format( "the package cannot be named `{}`", name ), *error );
        }
        else
        {
            info.name = name;
        }
    }
    return info;
}

bool holds_keel( const std::filesystem::path& folder )
{
    std::error_code ignored;
    for( const auto& entry : std::filesystem::directory_iterator( folder, ignored ) )
    {
        if( entry.is_regular_file( ignored ) && entry.path().extension() == ".kl" )
        {
            return true;
        }
    }
    return false;
}

// Each subfolder of `root` that directly holds a `.kl` is the package `parent::name`, and so on below it.
void add_nested_packages(
    Symbol_id                                       parent,
    const std::filesystem::path&                    root,
    Module_graph&                                   modules,
    std::unordered_map<u32, std::filesystem::path>& roots,
    Source_manager&                                 sm,
    Interner&                                       interner,
    Diagnostics&                                    diags
)
{
    std::vector<std::filesystem::path> folders;
    std::error_code                    ignored;
    for( const auto& entry : std::filesystem::directory_iterator( root, ignored ) )
    {
        const std::string name = entry.path().filename().generic_string();
        if( entry.is_directory( ignored ) && !package_name_error( name, interner ) && holds_keel( entry.path() ) )
        {
            folders.push_back( entry.path() );
        }
    }
    std::ranges::sort( folders );

    for( const std::filesystem::path& folder : folders )
    {
        const std::string name    = folder.filename().generic_string();
        const Symbol_id   package = interner.intern( qualified( interner, parent, name ) );

        if( const std::optional<File_id> module = sm.load_file( root / ( name + ".kl" ) ) )
        {
            diags.error(
                Span { *module, 0, 0 },
                fmt::format( "the module `{0}` and the package `{0}` share a name", interner.text( package ) ),
                "rename one of them"
            );
            continue;
        }

        modules.add_package( package );
        roots[package.v] = folder;
        add_nested_packages( package, folder, modules, roots, sm, interner, diags );
    }
}

// The program's subfolder `sub/deeper` is the package `app::sub::deeper` when each level holds a `.kl`,
// `skip` being the length of `app::`, or `sub::deeper` when the program has no name. Added when
// `import` first names it, so a folder nothing imports is never looked at. False when refused.
bool add_program_package(
    Symbol_id                                       package,
    std::size_t                                     skip,
    const std::filesystem::path&                    program_folder,
    Span                                            at,
    std::span<const Package>                        given,
    Module_graph&                                   modules,
    std::unordered_map<u32, std::filesystem::path>& roots,
    const Interner&                                 interner,
    Diagnostics&                                    diags
)
{
    const std::string     text   = std::string( interner.text( package ) );
    std::filesystem::path folder = program_folder;
    for( std::size_t begin = skip; begin <= text.size(); )
    {
        const std::size_t end   = std::min( text.find( "::", begin ), text.size() );
        const std::string piece = text.substr( begin, end - begin );
        const std::string name  = text.substr( 0, end );
        std::error_code   ignored;
        if( std::filesystem::is_regular_file( folder / ( piece + ".kl" ), ignored ) &&
            std::filesystem::is_directory( folder / piece, ignored ) )
        {
            diags.error( at, fmt::format( "the module `{0}` and the package `{0}` share a name", name ), "rename one of them" );
            modules.add_missing_package( package );
            return false;
        }

        folder /= piece;
        if( !holds_keel( folder ) )
        {
            return true;
        }

        // A `--package`'s own scan found what is below it; the build tool gives `src/sub` so.
        if( const auto it = std::ranges::find( given, name, &Package::name ); skip == 0 && begin == 0 && it != given.end() )
        {
            std::error_code same;
            if( std::filesystem::equivalent( it->root, folder, same ) )
            {
                return true;
            }
            diags.error( at, fmt::format( "the folder `{0}` and the package `{0}` share a name", name ), "rename the folder" );
            return false;
        }
        begin = end + 2;
    }

    modules.add_package( package );
    roots[package.v] = folder;
    return true;
}

} // namespace

Program load_program(
    File_id                  input,
    Source_manager&          sm,
    Interner&                interner,
    Literal_pool&            literals,
    Diagnostics&             diags,
    std::span<const Package> packages,
    bool                     as_module,
    std::span<const Object>  objects,
    std::string_view         prelude,
    std::string_view         prelude_name
)
{
    const File_id prelude_file = sm.add_file( std::string( prelude_name ), std::string( prelude ) );

    // A stack: the prelude is parsed last, so the program's node ids do not move as it grows.
    std::vector<File_id>    pending { prelude_file, input };
    std::unordered_set<u32> loaded;
    loaded.insert( prelude_file.v );
    loaded.insert( input.v );

    const std::filesystem::path program_folder = std::filesystem::path( sm.file( input ).path ).parent_path();

    Program                               prog;
    Ast&                                  ast          = prog.ast;
    Module_graph&                         modules      = prog.modules;
    std::unordered_map<u32, std::string>& package_docs = prog.package_docs;
    modules.set_prelude( prelude_file, interner.intern( "prelude" ) );

    const Package_info program_info = read_package_info( program_folder, sm, interner, literals, diags );

    std::unordered_map<u32, std::filesystem::path> package_roots;
    for( const Package& package : packages )
    {
        const Symbol_id package_id = interner.intern( package.name );
        modules.add_package( package_id );

        package_roots[package_id.v] = package.root;
        add_nested_packages( package_id, package.root, modules, package_roots, sm, interner, diags );
    }

    // An input in a package's folder is that package's module, never a program: so the editor
    // checks `kl/list.kl` alone as `kl::list`.
    bool in_package = false;
    for( const auto& [package_id, root] : package_roots )
    {
        std::error_code same;
        if( std::filesystem::equivalent( program_folder, root, same ) )
        {
            modules.place( input, Symbol_id { package_id } );
            package_docs[package_id] = program_info.doc; // the same folder's, already read
            in_package               = true;
        }
    }

    // The program's own package: the name its folder's packageinfo.kl declares, else the folder's.
    // None when that cannot name a package, as for `my-tool/`.
    Symbol_id program_package;
    if( !modules.package_of( input ).is_valid() )
    {
        const std::string name = program_info.name.value_or( folder_package_name( program_folder ) );
        const Symbol_id   id   = package_name_error( name, interner ) ? Symbol_id {} : interner.intern( name );
        if( id.is_valid() && modules.is_package( id ) )
        {
            diags.error(
                Span { input, 0, 0 },
                fmt::format(
                    "the program and the package in `{}` are both named `{}`", package_roots[id.v].generic_string(), name
                ),
                "give the program another name in its folder's `packageinfo.kl`, `@package( name );`"
            );
        }
        else if( id.is_valid() )
        {
            modules.add_package( id );
            modules.place( input, id );
            modules.set_program_package( id );
            package_roots[id.v] = program_folder;
            program_package     = id;
        }
    }
    package_docs[program_package.v] = program_info.doc;

    const bool        is_program_package = !modules.package_of( input ).is_valid();
    const std::string below_program = program_package.is_valid() ? std::string( interner.text( program_package ) ) + "::" : "";
    std::unordered_set<u32> program_packages;

    if( !as_module && !in_package )
    {
        modules.set_program( input );
    }

    // Each object's file holds its source text, for spans, and is never read from disk.
    std::unordered_map<std::string, File_id> object_files;
    for( const Object& object : objects )
    {
        const std::optional<Source_header> source = read_source( object.state );
        const File_id file = sm.add_file( source ? source->path : object.path, source ? source->text : std::string() );

        const std::string_view module    = object.manifest.module;
        const std::size_t      separator = module.rfind( "::" );
        const Symbol_id        package =
            separator == std::string_view::npos ? Symbol_id {} : interner.intern( module.substr( 0, separator ) );

        if( package.is_valid() && !modules.is_package( package ) )
        {
            modules.add_package( package );
        }
        modules.place( file, package );
        modules.set_loaded( file );
        object_files.emplace( module, file );
    }

    std::vector<Node_id> decls;

    while( !pending.empty() )
    {
        const File_id current = pending.back();
        pending.pop_back();

        // `packageinfo.kl` as the input: read_package_info has checked it, and it declares nothing.
        if( current == input && std::filesystem::path( sm.file( input ).path ).filename() == "packageinfo.kl" )
        {
            prog.first_node[current.v] = narrow_cast<u32>( ast.node_count() );
            continue;
        }

        const std::vector<Token> tokens       = lex( current, sm, interner, literals, diags );
        prog.first_node[current.v]            = narrow_cast<u32>( ast.node_count() );
        const std::vector<Node_id> file_decls = parse_into( ast, tokens, sm, diags );

        std::unordered_map<std::string, Span> imported;

        for( const Node_id decl : file_decls )
        {
            if( ast.kind( decl ) == Node_kind::Import_decl )
            {
                Symbol_id              package_id     = ast.package( decl ).is_valid()
                                                            ? interner.intern( package_text( ast, interner, ast.package( decl ) ) )
                                                            : modules.package_of( current );
                const std::string_view module_name    = interner.text( ast.name( decl ) );
                const std::string      qualified_name = qualified( interner, package_id, module_name );
                // As diagnostics write it: the program's own package is left off.
                const bool        in_program = package_id.is_valid() && package_id == program_package;
                const std::string shown_name = in_program ? std::string( module_name ) : qualified_name;

                // Reported, then loaded as written, so the names it brings still resolve.
                if( ast.package( decl ).is_valid() && package_id == modules.package_of( current ) )
                {
                    diags.error(
                        ast.span( decl ),
                        fmt::format( "`{}` is in this file's own package", qualified_name ),
                        fmt::format( "write `import {};`", module_name )
                    );
                }

                if( const Node_id alias = ast.import_alias( decl ); alias.is_valid() )
                {
                    const Symbol_id name  = ast.name( alias );
                    const Symbol_id other = modules.alias_package( current, name );
                    if( modules.is_package( name ) )
                    {
                        diags.error(
                            ast.span( alias ),
                            fmt::format( "`{}` is already the name of a package", interner.text( name ) ),
                            "choose another"
                        );
                    }
                    else if( other.is_valid() && other != package_id )
                    {
                        diags.error(
                            ast.span( alias ),
                            fmt::format(
                                "`{}` already names the package `{}` here", interner.text( name ), interner.text( other )
                            ),
                            "choose another"
                        );
                    }
                    else if( !other.is_valid() )
                    {
                        modules.add_alias( current, { name, package_id, ast.span( alias ) } );
                    }
                }

                if( const auto [first, fresh] = imported.try_emplace( qualified_name, ast.span( decl ) ); !fresh )
                {
                    const Line_col at = sm.line_col( first->second.file, first->second.start );
                    diags.error(
                        ast.span( decl ),
                        fmt::format( "`{}` is already imported", shown_name ),
                        fmt::format(
                            "previous import is at: {}:{}:{}; remove this one", sm.file( current ).path, at.line, at.col
                        )
                    );
                    continue;
                }

                if( const auto it = object_files.find( qualified_name ); it != object_files.end() )
                {
                    modules.add( current, it->second, ast.span( decl ) );
                    continue;
                }

                if( as_module )
                {
                    diags.error(
                        ast.span( decl ),
                        fmt::format( "`{}` is imported, but no object for it was given", shown_name ),
                        "build it with `-c` first, and give the `.klo`"
                    );
                    continue;
                }

                std::filesystem::path module_folder = program_folder;
                if( package_id.is_valid() )
                {
                    const bool under_program =
                        !below_program.empty() && interner.text( package_id ).starts_with( below_program );
                    if( ( is_program_package || under_program ) && !package_roots.contains( package_id.v ) &&
                        program_packages.insert( package_id.v ).second &&
                        !add_program_package(
                            package_id,
                            under_program ? below_program.size() : 0,
                            program_folder,
                            ast.span( decl ),
                            packages,
                            modules,
                            package_roots,
                            interner,
                            diags
                        ) )
                    {
                        continue;
                    }
                    if( const auto it = package_roots.find( package_id.v ); it != package_roots.end() )
                    {
                        module_folder = it->second;
                        // Only a package something imports is documented, so an unused kl costs nothing.
                        if( !package_docs.contains( package_id.v ) )
                        {
                            const std::string_view text = interner.text( package_id );
                            package_docs[package_id.v] =
                                read_package_info(
                                    module_folder,
                                    sm,
                                    interner,
                                    literals,
                                    diags,
                                    text.find( "::" ) != std::string_view::npos ? text : std::string_view {}
                                )
                                    .doc;
                        }
                    }
                    else
                    {
                        if( !modules.is_missing( package_id ) )
                        {
                            diags.error(
                                ast.span( decl ),
                                fmt::format( "there is no package `{}`", interner.text( package_id ) ),
                                interner.text( package_id ).find( "::" ) != std::string_view::npos
                                    ? std::string( "a nested package is a subfolder of its parent that holds `.kl` files" )
                                    : std::string( "a package is a subfolder holding `.kl` files, or a folder given with "
                                                   "`--package <dir>`" )
                            );
                        }
                        modules.add_missing_package( package_id );
                        continue;
                    }
                }

                if( module_name == "packageinfo" )
                {
                    diags.error(
                        ast.span( decl ),
                        "`packageinfo` is not a module",
                        "`packageinfo.kl` holds the package's documentation, which nothing imports"
                    );
                    continue;
                }

                const std::filesystem::path module_path = module_folder / ( std::string( module_name ) + ".kl" );

                // Dedupes by canonical path, which is what stops cycles and diamonds
                const std::optional<File_id> module = sm.load_file( module_path );

                if( !module.has_value() )
                {
                    diags.error(
                        ast.span( decl ),
                        fmt::format( "there is no module `{}`", shown_name ),
                        package_id.is_valid() && !in_program
                            ? fmt::format( "it would be `{}.kl` in the package `{}`", module_name, interner.text( package_id ) )
                            : fmt::format(
                                  "`import {};` looks for `{}.kl` beside the file being compiled", module_name, module_name
                              )
                    );
                    continue;
                }

                modules.add( current, *module, ast.span( decl ) );
                if( loaded.insert( module->v ).second )
                {
                    modules.place( *module, package_id );
                    pending.push_back( *module );
                }
            }
            else
            {
                decls.push_back( decl );
            }
        }
    }

    // A `--package` outside the input's folder is someone else's, so its warnings are not this build's.
    const std::filesystem::path base       = std::filesystem::absolute( program_folder.empty() ? "." : program_folder );
    const Symbol_id             own        = modules.package_of( input );
    const auto                  is_outside = [&]( const std::filesystem::path& root )
    {
        std::error_code             ignored;
        const std::filesystem::path within = std::filesystem::relative( root, base, ignored );
        return within.empty() || *within.begin() == "..";
    };
    for( const u32 file : loaded )
    {
        const Symbol_id package = modules.package_of( File_id { file } );
        const auto      root    = package_roots.find( package.v );
        if( package.is_valid() && package != own && root != package_roots.end() && is_outside( root->second ) )
        {
            diags.quiet( File_id { file } );
        }
    }

    // After the prelude, which every state names, and each object after the objects it was built against.
    std::vector<const Object*> order;
    const auto                 visit = [&]( auto&& self, const Object& object ) -> void
    {
        if( std::ranges::find( order, &object ) != order.end() )
        {
            return;
        }
        for( const auto& [dependency, hash] : object.manifest.depends )
        {
            if( const auto it = std::ranges::find( objects, dependency, &Object::module ); it != objects.end() )
            {
                self( self, *it );
            }
        }
        order.push_back( &object );
    };
    for( const Object& object : objects )
    {
        visit( visit, object );
    }

    for( const Object* object : order )
    {
        const File_id               file  = object_files.at( object->manifest.module );
        std::optional<Loaded_state> state = read_state( object->state, file, prog, interner, literals, prog.types, sm );

        // The build id matched, so this keelc wrote it.
        if( !state )
        {
            diags.error(
                Span { file, 0, 0 },
                fmt::format( "keelc cannot read its own object '{}'", object->path ),
                "this is a bug in keelc"
            );
            continue;
        }

        for( const auto& [dependency, hash] : object->manifest.depends )
        {
            if( const auto it = object_files.find( dependency ); it != object_files.end() )
            {
                modules.add( file, it->second );
            }
        }

        state->object = object->path;
        state->level  = object->manifest.debug ? std::optional( object->manifest.level ) : std::nullopt;
        state->cpu    = object->manifest.cpu;
        decls.insert( decls.end(), state->declarations.begin(), state->declarations.end() );
        prog.states.push_back( std::move( *state ) );
    }

    std::vector<std::pair<File_id, File_id>> closing_imports = modules.closing_imports( input );
    for( const auto& [from, to] : closing_imports )
    {
        const auto name = [&]( File_id file ) { return shown_module_name( modules, sm, interner, file ); };

        if( from == to )
        {
            diags.error(
                modules.import_span( from, to ), fmt::format( "`{}` imports itself", name( from ) ), "remove this import"
            );
            continue;
        }

        // `to` imports ... imports `from`, whose import of `to` closes the loop.
        const std::vector<File_id> chain = modules.cycle( from, to );

        std::string loop = fmt::format( "`{}` imports `{}`", name( chain[0] ), name( chain[1] ) );
        for( std::size_t i = 2; i < chain.size(); ++i )
        {
            loop += fmt::format( ", which imports `{}`", name( chain[i] ) );
        }

        diags.error(
            modules.import_span( from, to ),
            fmt::format( "importing `{}` closes a cycle: {}", name( to ), loop ),
            "move what both modules need into a third, which each of them imports"
        );
    }

    const Node_id root =
        ast.add( Node_kind::Source_file, Span { input, 0, narrow_cast<u32>( sm.file( input ).text.size() ) }, 0, decls );
    ast.set_root( root );

    return prog;
}

std::optional<std::string> declared_package_name( const std::filesystem::path& folder )
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;

    const std::optional<File_id> file = sm.load_file( folder / "packageinfo.kl" );
    if( !file.has_value() )
    {
        return std::nullopt;
    }
    const Package_header header = scan_package_header( lex( *file, sm, interner, literals, diags ), sm );
    return header.name && !header.wrong ? std::optional( std::string( sm.text( *header.name ) ) ) : std::nullopt;
}

std::string folder_package_name( const std::filesystem::path& folder )
{
    std::error_code       ignored;
    std::filesystem::path path = std::filesystem::weakly_canonical( std::filesystem::absolute( folder, ignored ), ignored );
    if( path.filename().empty() )
    {
        path = path.parent_path();
    }
    // A project's `src/` and `test/` are the project, so each is named for the folder holding it.
    if( ( path.filename() == "src" || path.filename() == "test" ) && path.has_parent_path() )
    {
        path = path.parent_path();
    }
    return path.filename().generic_string();
}

std::optional<std::string> package_name_error( std::string_view name, const Interner& interner )
{
    if( name == "prelude" )
    {
        return "cannot name a package 'prelude', which is the prelude's own";
    }

    if( interner.is_keyword( interner.find( name ) ) )
    {
        return fmt::format( "name '{}' is a keyword", name );
    }

    if( !is_identifier( name ) )
    {
        return fmt::format( "name '{}' is not an identifier", name );
    }

    return std::nullopt;
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

// A program on disk: `main` and whatever else it names, loaded the way the driver loads one. The
// package `kl` is the directory `kl/`.
class Loaded
{
public:
    explicit Loaded(
        std::initializer_list<std::pair<const char*, std::string_view>> files,
        std::string_view                                                prelude = {},
        std::string_view                                                main    = "main.kl"
    )
    {
        for( const auto& [name, text] : files )
        {
            dir_.write( name, text );
        }

        const std::optional<File_id> input = sm_.load_file( dir_.path / main );

        REQUIRE( input.has_value() );
        const Package kl { .name = "kl", .root = dir_.path / "kl" };
        program_ = load_program( *input, sm_, interner_, literals_, diags_, std::span( &kl, 1 ), false, {}, prelude );

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
        return program_.modules.sees( files_.at( std::string( from ) ), files_.at( std::string( to ) ) );
    }

    // The package `file` was loaded from; empty when it has none.
    std::string_view package_of( std::string_view file ) const
    {
        const Symbol_id package = program_.modules.package_of( files_.at( std::string( file ) ) );
        return package.is_valid() ? interner_.text( package ) : std::string_view {};
    }

    // The name the program's folder gives it: the temporary folder's, unless declared.
    std::string_view program_package() const
    {
        const Symbol_id package = program_.modules.program_package();
        return package.is_valid() ? interner_.text( package ) : std::string_view {};
    }

    bool is_package( std::string_view name )
    {
        return program_.modules.is_package( interner_.intern( name ) );
    }

    bool has_program() const
    {
        return program_.modules.has_program();
    }

    // The package's `//!` documentation; empty names the program's own package.
    std::string package_doc( std::string_view package )
    {
        const Symbol_id id    = package.empty() ? Symbol_id {} : interner_.intern( package );
        const auto      found = program_.package_docs.find( id.v );
        return found == program_.package_docs.end() ? std::string {} : found->second;
    }

    std::size_t errors() const
    {
        return diags_.error_count();
    }

    // Whether a warning in `file` would be shown.
    bool warns_in( std::string_view file )
    {
        diags_.warning( Warning::Unused_variable, Span { files_.at( std::string( file ) ), 0, 0 }, "probe" );
        return rendered().find( "probe" ) != std::string::npos;
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
            { "a.kl", "import b;\ni32 fa() { return fb(); }\n" },
            { "b.kl", "i32 fb() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );

        REQUIRE( p.sees( "main.kl", "main.kl" ) );
        REQUIRE( p.sees( "main.kl", "a.kl" ) );
        REQUIRE_FALSE( p.sees( "main.kl", "b.kl" ) );

        REQUIRE( p.sees( "a.kl", "b.kl" ) );
        REQUIRE_FALSE( p.sees( "a.kl", "main.kl" ) );

        REQUIRE( p.sees( "b.kl", "b.kl" ) );
        REQUIRE_FALSE( p.sees( "b.kl", "a.kl" ) );
    }

    // A diamond reaches `c` twice. Each file is read once.
    SECTION( "a module is loaded once" )
    {
        const Loaded p( {
            { "main.kl", "import a;\nimport b;\ni32 main() { return fa() + fb(); }\n" },
            { "a.kl", "import c;\ni32 fa() { return fc(); }\n" },
            { "b.kl", "import c;\ni32 fb() { return fc(); }\n" },
            { "c.kl", "i32 fc() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "fa", "fb", "fc", "main" } );
    }

    SECTION( "an import that closes a cycle is refused" )
    {
        const Loaded p( {
            { "main.kl", "import a;\nimport b;\ni32 main() { return fa() + fb(); }\n" },
            { "a.kl", "import c;\ni32 fa() { return fc(); }\n" },
            { "b.kl", "import c;\ni32 fb() { return fc(); }\n" },
            { "c.kl", "import main;\ni32 fc() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE(
            p.rendered().find( "importing `main` closes a cycle: `main` imports `a`, which imports `c`" ) != std::string::npos
        );
        REQUIRE( p.rendered().find( "c.kl:1:1" ) != std::string::npos );
    }

    SECTION( "a module that imports itself is refused" )
    {
        const Loaded p( {
            { "main.kl", "import a;\ni32 main() { return fa(); }\n" },
            { "a.kl", "import a;\ni32 fa() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`a` imports itself" ) != std::string::npos );
        REQUIRE( p.rendered().find( "a.kl:1:1" ) != std::string::npos );
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
        REQUIRE( p.package_of( "main.kl" ) == p.program_package() );
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

    // Refused, and still loaded, so its names resolve.
    SECTION( "a package naming itself is refused" )
    {
        const Loaded p( {
            { "main.kl", "import kl::geom;\ni32 main() { return 0; }\n" },
            { "kl/geom.kl", "import kl::shape;\ni32 area() { return side(); }\n" },
            { "kl/shape.kl", "i32 side() { return 2; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`kl::shape` is in this file's own package" ) != std::string::npos );
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
        REQUIRE( p.package_of( "geom.kl" ) == p.program_package() );
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

    SECTION( "an unknown nested package is reported whole" )
    {
        const Loaded p( {
            { "main.kl", "import kl::geom::shapes;\ni32 main() { return 0; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "there is no package `kl::geom`" ) != std::string::npos );
    }

    SECTION( "a subfolder of a package is a package of its own" )
    {
        const Loaded p( {
            { "main.kl", "import kl::test::context;\ni32 main() { return 0; }\n" },
            { "kl/test/context.kl", "i32 run() { return 0; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "main", "run" } );
        REQUIRE( p.package_of( "kl/test/context.kl" ) == "kl::test" );
    }

    SECTION( "a bare import in a nested package looks in it" )
    {
        const Loaded p( {
            { "main.kl", "import kl::test::a;\ni32 main() { return 0; }\n" },
            { "kl/b.kl", "i32 decoy() { return 1; }\n" },
            { "kl/test/a.kl", "import b;\ni32 f() { return g(); }\n" },
            { "kl/test/b.kl", "i32 g() { return 2; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "f", "g", "main" } );
        REQUIRE( p.package_of( "kl/test/b.kl" ) == "kl::test" );
    }

    SECTION( "a nested package reaches its parent only by name" )
    {
        const Loaded p( {
            { "main.kl", "import kl::test::a;\ni32 main() { return 0; }\n" },
            { "kl/b.kl", "i32 g() { return 2; }\n" },
            { "kl/test/a.kl", "import kl::b;\ni32 f() { return g(); }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.functions() == std::vector<std::string_view> { "f", "g", "main" } );
        REQUIRE( p.package_of( "kl/b.kl" ) == "kl" );
    }

    SECTION( "a folder holding no `.kl` is not a package" )
    {
        Loaded p( {
            { "main.kl", "i32 main() { return 0; }\n" },
            { "kl/assets/x.txt", "not keel\n" },
        } );

        REQUIRE( p.errors() == 0 );
        REQUIRE_FALSE( p.is_package( "kl::assets" ) );
    }

    SECTION( "a module and a folder of one name are refused" )
    {
        Loaded p( {
            { "main.kl", "i32 main() { return 0; }\n" },
            { "kl/test.kl", "i32 f() { return 0; }\n" },
            { "kl/test/a.kl", "i32 g() { return 0; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "the module `kl::test` and the package `kl::test` share a name" ) != std::string::npos );
        REQUIRE( p.rendered().find( "kl/test.kl:1:1" ) != std::string::npos );
        REQUIRE_FALSE( p.is_package( "kl::test" ) );
    }

    SECTION( "a nested package two deep" )
    {
        const Loaded found( {
            { "main.kl", "import kl::a::b::c;\ni32 main() { return 0; }\n" },
            { "kl/a/x.kl", "i32 x() { return 0; }\n" },
            { "kl/a/b/c.kl", "i32 c() { return 0; }\n" },
        } );

        INFO( found.rendered() );
        REQUIRE( found.errors() == 0 );
        REQUIRE( found.package_of( "kl/a/b/c.kl" ) == "kl::a::b" );

        const Loaded skipped( {
            { "main.kl", "import kl::a::b::c;\ni32 main() { return 0; }\n" },
            { "kl/a/b/c.kl", "i32 c() { return 0; }\n" },
        } );

        REQUIRE( skipped.errors() == 1 );
        REQUIRE( skipped.rendered().find( "there is no package `kl::a::b`" ) != std::string::npos );
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

// D53: a package's documentation is its `packageinfo.kl`, which holds nothing but comments.
// A `--package` beside the program's folder is another author's; one inside it is the program's own.
TEST_CASE( "loader_quiets_a_package_outside_the_program's_folder", "[parse][loader][packages]" )
{
    SECTION( "outside" )
    {
        Loaded p(
            {
                { "app/main.kl", "import kl::geom;\ni32 main() { return 0; }\n" },
                { "kl/geom.kl", "i32 area() { return 4; }\n" },
            },
            {},
            "app/main.kl"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( !p.warns_in( "kl/geom.kl" ) );
        REQUIRE( p.warns_in( "app/main.kl" ) );
    }

    SECTION( "inside" )
    {
        Loaded p( {
            { "main.kl", "import kl::geom;\ni32 main() { return 0; }\n" },
            { "kl/geom.kl", "i32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.warns_in( "kl/geom.kl" ) );
    }
}

// The editor checks a library file alone: it is its package's module, never a program.
TEST_CASE( "loader_reads_an_input_in_a_package's_folder_as_its_module", "[parse][loader][packages]" )
{
    SECTION( "a module, which reaches its siblings through its package" )
    {
        Loaded p(
            {
                { "kl/geom.kl", "import shape;\ni32 area() { return kl::side() * 2; }\n" },
                { "kl/shape.kl", "i32 side() { return 2; }\n" },
                { "kl/packageinfo.kl", "//! The kl package.\n@package( kl );\n" },
            },
            {},
            "kl/geom.kl"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.package_of( "kl/geom.kl" ) == "kl" );
        REQUIRE_FALSE( p.has_program() );
    }

    SECTION( "its `packageinfo.kl`, which declares nothing" )
    {
        Loaded p( { { "kl/packageinfo.kl", "//! The kl package.\n@package( kl );\n" } }, {}, "kl/packageinfo.kl" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "and a broken one is still reported" )
    {
        Loaded p( { { "kl/packageinfo.kl", "//! The kl package.\n@package( );\n" } }, {}, "kl/packageinfo.kl" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

TEST_CASE( "loader_reads_a_package's_packageinfo", "[parse][loader][packages][docs]" )
{
    constexpr std::string_view info = "// Copyright\n//! The kl package.\n//!\n//! More.\n";

    SECTION( "a package's `//!` lines are its documentation" )
    {
        Loaded p( {
            { "main.kl", "import kl::geom;\ni32 main() { return 0; }\n" },
            { "kl/geom.kl", "i32 area() { return 4; }\n" },
            { "kl/packageinfo.kl", info },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.package_doc( "kl" ) == "The kl package.\n\nMore." );
        REQUIRE( p.package_doc( "" ).empty() );
    }

    SECTION( "and so are the program's own" )
    {
        Loaded p( {
            { "main.kl", "i32 main() { return 0; }\n" },
            { "packageinfo.kl", "//! The program.\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.package_doc( p.program_package() ) == "The program." );
    }

    SECTION( "the program's packageinfo.kl may name it" )
    {
        Loaded p( {
            { "main.kl", "import tool::sub::leaf;\ni32 main() { return tool::sub::one(); }\n" },
            { "packageinfo.kl", "//! The program.\n@package( tool );\n" },
            { "sub/leaf.kl", "i32 one() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.program_package() == "tool" );
        REQUIRE( p.package_of( "sub/leaf.kl" ) == "tool::sub" );
        REQUIRE( p.package_doc( "tool" ) == "The program." );
    }

    SECTION( "a package without one has none" )
    {
        Loaded p( {
            { "main.kl", "import kl::geom;\ni32 main() { return 0; }\n" },
            { "kl/geom.kl", "i32 area() { return 4; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.package_doc( "kl" ).empty() );
    }

    SECTION( "a package nothing imports is never read" )
    {
        const Loaded p( {
            { "main.kl", "i32 main() { return 0; }\n" },
            { "kl/packageinfo.kl", "i32 stray() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    // One error for the file, however much is in it.
    SECTION( "anything but comments in it is refused" )
    {
        const Loaded p( {
            { "main.kl", "import kl::geom;\ni32 main() { return 0; }\n" },
            { "kl/geom.kl", "i32 area() { return 4; }\n" },
            { "kl/packageinfo.kl", "//! The kl package.\nimport geom;\ni32 stray() { return 1; }\n" },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`packageinfo.kl` holds only comments" ) != std::string::npos );
        REQUIRE( p.rendered().find( "packageinfo.kl:2:1" ) != std::string::npos );
    }

    SECTION( "it is not a module, so nothing imports it" )
    {
        const Loaded p( {
            { "main.kl", "import kl::packageinfo;\nimport packageinfo;\ni32 main() { return 0; }\n" },
            { "packageinfo.kl", "//! The program.\n" },
            { "kl/packageinfo.kl", info },
        } );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "`packageinfo` is not a module" ) != std::string::npos );
        REQUIRE( p.rendered().find( "main.kl:1:1" ) != std::string::npos );
        REQUIRE( p.rendered().find( "main.kl:2:1" ) != std::string::npos );
    }
}

TEST_CASE( "loader_refuses_a_package_name_that_cannot_be_written", "[parse][loader][packages]" )
{
    const Interner interner;

    REQUIRE_FALSE( package_name_error( "kl", interner ).has_value() );
    REQUIRE( package_name_error( "prelude", interner ).value().find( "prelude's own" ) != std::string::npos );
    REQUIRE( package_name_error( "struct", interner ).value() == "name 'struct' is a keyword" );
    REQUIRE( package_name_error( "my-lib", interner ).value() == "name 'my-lib' is not an identifier" );
}

TEST_CASE( "module_graph_orders_each_file_after_its_imports", "[parse][modules]" )
{
    const File_id main { 0 }, a { 1 }, b { 2 }, unreached { 3 }, prelude { 4 };

    Module_graph graph;
    graph.set_prelude( prelude, Symbol_id {} );
    graph.add( main, a );
    graph.add( main, b );
    graph.add( a, b );
    graph.add( unreached, main );

    SECTION( "the prelude first, the input last, and nothing the input does not reach" )
    {
        REQUIRE( graph.dependencies_first( main ) == std::vector<File_id> { prelude, b, a, main } );
    }

    SECTION( "an explicit import of the prelude lists it once" )
    {
        graph.add( b, prelude );
        REQUIRE( graph.dependencies_first( main ) == std::vector<File_id> { prelude, b, a, main } );
    }

    SECTION( "a cycle is cut where the walk meets a file it is still inside" )
    {
        graph.add( b, main );
        REQUIRE( graph.dependencies_first( main ) == std::vector<File_id> { prelude, b, a, main } );
        REQUIRE( graph.dependencies_first( a ) == std::vector<File_id> { prelude, main, b, a } );
    }
}

TEST_CASE( "module_graph_finds_the_imports_that_close_a_cycle", "[parse][modules]" )
{
    const File_id main { 0 }, a { 1 }, b { 2 }, prelude { 3 };

    Module_graph graph;
    graph.set_prelude( prelude, Symbol_id {} );
    graph.add( main, a );
    graph.add( main, b );
    graph.add( a, b );

    using Edges = std::vector<std::pair<File_id, File_id>>;

    SECTION( "a diamond closes none" )
    {
        REQUIRE( graph.closing_imports( main ).empty() );
    }

    SECTION( "an import back to a file the walk is inside closes one" )
    {
        graph.add( b, main );
        REQUIRE( graph.closing_imports( main ) == Edges { { b, main } } );
        REQUIRE( graph.cycle( b, main ) == std::vector<File_id> { main, a, b } );
    }

    SECTION( "an import of itself closes one" )
    {
        graph.add( a, a );
        REQUIRE( graph.closing_imports( main ) == Edges { { a, a } } );
    }
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
