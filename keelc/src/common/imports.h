// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <algorithm>
#include <vector>
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

private:
    // The graph is sparse, so an adjacency list is better than a matrix.
    std::vector<std::vector<File_id>> edges_;
};
} // namespace keel