// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ast/ast.h"
#include "ast/node.h"
#include "common/diagnostics.h"
#include "common/imports.h"
#include "common/interner.h"
#include "common/source_manager.h"

namespace keel
{

// Which declaration each use refers to, indexed by the use's Node_id. A side table rather than a
// field on Node: aux still holds the Symbol_id that diagnostics need, and the AST stays immutable
// after parsing.
class Resolution
{
public:
    Resolution() = default;
    Resolution( std::vector<Node_id> bindings, std::vector<Node_id> next_overload, const Imports& imports )
        : bindings_( std::move( bindings ) ),
          next_overload_( std::move( next_overload ) ),
          imports_( imports )
    {
    }

    // Invalid when the use was never resolved - an unknown name, or a node that is not a use. With
    // overloading this is the *first* candidate: the rest follow it through next_overload.
    Node_id declaration_of( Node_id use ) const
    {
        return use.v < bindings_.size() ? bindings_[use.v] : Node_id {};
    }

    // The next declaration of the same name in the same scope, or invalid at the end of the chain.
    // Per-declaration rather than per-use: a use has one name, and the set belongs to the name.
    Node_id next_overload( Node_id declaration ) const
    {
        return declaration.v < next_overload_.size() ? next_overload_[declaration.v] : Node_id {};
    }

    bool sees( File_id from, File_id to ) const
    {
        return imports_.sees( from, to );
    }

private:
    std::vector<Node_id> bindings_;
    std::vector<Node_id> next_overload_;
    Imports              imports_;
};

Resolution
resolve( const Ast& ast, const Source_manager& sm, const Interner& interner, Diagnostics& diags, const Imports& imports = {} );

} // namespace keel
