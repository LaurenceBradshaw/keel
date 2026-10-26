// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "common/span.h"

namespace keel
{
// Which file sees which, which names are packages, which package each file is in, and which file is
// the prelude. The loader builds it.
class Module_graph
{
public:
    // The import graph.
    void add( File_id from, File_id to, Span span = {} )
    {
        import_spans_.try_emplace( edge( from, to ), span );

        if( edges_.size() <= from.v )
        {
            edges_.resize( from.v + 1 );
        }

        std::vector<File_id>& adj = edges_[from.v];
        if( std::find( adj.begin(), adj.end(), to ) == adj.end() )
        {
            adj.push_back( to );
            std::sort( adj.begin(), adj.end() );
        }
    }

    bool sees( File_id from, File_id to ) const
    {
        if( to == prelude_ )
        {
            return true;
        }

        if( from == to )
        {
            return true;
        }

        if( from.v >= edges_.size() )
        {
            return false;
        }

        const std::vector<File_id>& adj = edges_[from.v];
        return std::binary_search( adj.begin(), adj.end(), to );
    }

    // Where `from` wrote its `import` of `to`.
    Span import_span( File_id from, File_id to ) const
    {
        const auto it = import_spans_.find( edge( from, to ) );
        return it != import_spans_.end() ? it->second : Span {};
    }

    // Every file that imports both.
    std::vector<File_id> common_importers( File_id a, File_id b ) const
    {
        std::vector<File_id> importers;
        for( u32 from = 0; from < edges_.size(); ++from )
        {
            const std::vector<File_id>& adj = edges_[from];
            if( std::binary_search( adj.begin(), adj.end(), a ) && std::binary_search( adj.begin(), adj.end(), b ) )
            {
                importers.push_back( File_id { from } );
            }
        }
        return importers;
    }

    // Every file `input` reaches, each after the files it imports, the prelude first since every file
    // sees it. A cycle, already refused, is cut where the walk meets a file it is still inside.
    std::vector<File_id> dependencies_first( File_id input ) const
    {
        std::vector<File_id> order;
        std::vector<File_id> seen;

        const auto visit = [&]( auto&& self, File_id file ) -> void
        {
            if( std::find( seen.begin(), seen.end(), file ) != seen.end() )
            {
                return;
            }

            seen.push_back( file );

            if( file.v < edges_.size() )
            {
                for( const File_id to : edges_[file.v] )
                {
                    self( self, to );
                }
            }

            order.push_back( file );
        };

        if( prelude_.is_valid() )
        {
            visit( visit, prelude_ );
        }

        visit( visit, input );
        return order;
    }

    // Each import that closes a cycle, as (importer, imported): an edge back to a file the walk from
    // `input` is still inside.
    std::vector<std::pair<File_id, File_id>> closing_imports( File_id input ) const
    {
        std::vector<std::pair<File_id, File_id>> closing;
        std::vector<File_id>                     inside;
        std::vector<File_id>                     done;

        const auto visit = [&]( auto&& self, File_id file ) -> void
        {
            inside.push_back( file );

            if( file.v < edges_.size() )
            {
                for( const File_id next : edges_[file.v] )
                {
                    if( std::find( inside.begin(), inside.end(), next ) != inside.end() )
                    {
                        closing.emplace_back( file, next );
                    }
                    else if( std::find( done.begin(), done.end(), next ) == done.end() )
                    {
                        self( self, next );
                    }
                }
            }

            inside.pop_back();
            done.push_back( file );
        };

        visit( visit, input );
        return closing;
    }

    // The imports from `to` that lead back to `from`: `to` first, `from` last.
    std::vector<File_id> cycle( File_id from, File_id to ) const
    {
        std::vector<File_id> path;
        std::vector<File_id> seen;

        const auto visit = [&]( auto&& self, File_id file ) -> bool
        {
            seen.push_back( file );
            path.push_back( file );

            if( file == from )
            {
                return true;
            }

            if( file.v < edges_.size() )
            {
                for( const File_id next : edges_[file.v] )
                {
                    if( std::find( seen.begin(), seen.end(), next ) == seen.end() && self( self, next ) )
                    {
                        return true;
                    }
                }
            }

            path.pop_back();
            return false;
        };

        visit( visit, to );
        return path;
    }

    // Packages.
    void add_package( Symbol_id package )
    {
        packages_.push_back( package );
    }

    // Imported but not named by `--package`: still a package, so `kl::` reads as one, but nothing in it resolves.
    void add_missing_package( Symbol_id package )
    {
        packages_.push_back( package );
        missing_packages_.push_back( package );
    }

    bool is_package( Symbol_id package ) const
    {
        return std::find( packages_.begin(), packages_.end(), package ) != packages_.end();
    }

    bool is_missing( Symbol_id package ) const
    {
        return std::find( missing_packages_.begin(), missing_packages_.end(), package ) != missing_packages_.end();
    }

    // Each file's package.
    void place( File_id file, Symbol_id package )
    {
        file_to_package_[file.v] = package.v;
    }

    Symbol_id package_of( File_id file ) const
    {
        if( const auto it = file_to_package_.find( file.v ); it != file_to_package_.end() )
        {
            return Symbol_id { it->second };
        }
        return Symbol_id {};
    }

    // The prelude.
    void set_prelude( File_id file, Symbol_id package )
    {
        prelude_         = file;
        prelude_package_ = package;
        place( file, package );
    }

    Symbol_id prelude_package() const
    {
        return prelude_package_;
    }

    File_id prelude_file() const
    {
        return prelude_;
    }

    // The program's file, whose names take no module; none when the input is compiled as a module.
    void set_program( File_id file )
    {
        program_ = file;
    }

    bool is_program( File_id file ) const
    {
        return program_.is_valid() && file == program_;
    }

    // A module read from an object: checked when it was built, so nothing checks it again.
    void set_loaded( File_id file )
    {
        loaded_.push_back( file );
    }

    bool is_loaded( File_id file ) const
    {
        return std::find( loaded_.begin(), loaded_.end(), file ) != loaded_.end();
    }

private:
    static u64 edge( File_id from, File_id to )
    {
        return ( u64( from.v ) << 32 ) | to.v;
    }

    // The graph is sparse, so an adjacency list is better than a matrix.
    std::vector<std::vector<File_id>> edges_;
    std::unordered_map<u64, Span>     import_spans_;
    std::vector<Symbol_id>            packages_;
    std::vector<Symbol_id>            missing_packages_;
    std::unordered_map<u32, u32>      file_to_package_;
    File_id                           prelude_;
    Symbol_id                         prelude_package_;
    File_id                           program_;
    std::vector<File_id>              loaded_;
};

// `name`, or `package::name` when there is a package. The program's own package has no name.
inline std::string qualified( const Interner& interner, Symbol_id package, std::string_view name )
{
    if( !package.is_valid() )
    {
        return std::string( name );
    }

    return std::string( interner.text( package ) ) + "::" + std::string( name );
}

// A package node's name, as `kl::test`: a Name_expr, or a Path_expr chain of them.
inline std::string package_text( const Ast& ast, const Interner& interner, Node_id package )
{
    if( ast.kind( package ) == Node_kind::Path_expr )
    {
        return package_text( ast, interner, ast.qualifier( package ) ) +
               "::" + std::string( interner.text( ast.name( package ) ) );
    }

    return std::string( interner.text( ast.name( package ) ) );
}

// A file's module, as `kl::ring`: its package and its stem.
inline std::string module_name( const Module_graph& modules, const Source_manager& sm, const Interner& interner, File_id file )
{
    return qualified( interner, modules.package_of( file ), std::filesystem::path( sm.file( file ).path ).stem().string() );
}
} // namespace keel
