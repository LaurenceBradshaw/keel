// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "common/interner.h"
#include "common/span.h"

namespace keel
{
class Imports
{
public:
    void add( File_id from, File_id to )
    {
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

private:
    // The graph is sparse, so an adjacency list is better than a matrix.
    std::vector<std::vector<File_id>> edges_;
    std::vector<Symbol_id>            packages_;
    std::vector<Symbol_id>            missing_packages_;
    std::unordered_map<u32, u32>      file_to_package_;
    File_id                           prelude_;
    Symbol_id                         prelude_package_;
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
} // namespace keel
