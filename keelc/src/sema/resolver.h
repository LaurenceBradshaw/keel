#pragma once
#include <vector>
#include "ast/ast.h"
#include "ast/node.h"
#include "common/diagnostics.h"
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
    explicit Resolution( std::vector<Node_id> bindings )
        : bindings_( std::move( bindings ) )
    {
    }

    // Invalid when the use was never resolved - an unknown name, or a node that is not a use.
    Node_id declaration_of( Node_id use ) const
    {
        return use.v < bindings_.size() ? bindings_[use.v] : Node_id {};
    }

private:
    std::vector<Node_id> bindings_;
};

Resolution resolve( const Ast& ast, const Source_manager& sm, const Interner& interner, Diagnostics& diags );

} // namespace keel