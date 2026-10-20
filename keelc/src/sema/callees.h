// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <unordered_map>
#include <vector>
#include "ast/node.h"
#include "sema/type.h"

namespace keel::sema
{

// Which callable each call resolved to. A collaborator rather than a field because Places reads it
// and Overloads writes it, and neither may name the other.
class Callees
{
public:
    Callees() = default;

    void record( Node_id call, Node_id callable )
    {
        callees_.emplace( call.v, callable );
    }

    Node_id callee_of( Node_id call ) const
    {
        const auto found = callees_.find( call.v );

        if( found == callees_.end() )
        {
            return Node_id {};
        }

        return found->second;
    }

    // The bound's instance an operator on a type parameter went through; it has no callee node.
    void record_through( Node_id use, Type_id interface )
    {
        through_.emplace( use.v, interface );
    }

    Type_id through_of( Node_id use ) const
    {
        const auto found = through_.find( use.v );

        return found == through_.end() ? Type_id {} : found->second;
    }

    // `@functions_of( T )`: the functions it lists, in load order.
    void record_listed( Node_id use, std::vector<Node_id> functions )
    {
        listed_.emplace( use.v, std::move( functions ) );
    }

    std::unordered_map<u32, Node_id> take()
    {
        return std::move( callees_ );
    }

    std::unordered_map<u32, Type_id> take_through()
    {
        return std::move( through_ );
    }

    std::unordered_map<u32, std::vector<Node_id>> take_listed()
    {
        return std::move( listed_ );
    }

private:
    std::unordered_map<u32, Node_id>              callees_;
    std::unordered_map<u32, Type_id>              through_;
    std::unordered_map<u32, std::vector<Node_id>> listed_;
};

} // namespace keel::sema
