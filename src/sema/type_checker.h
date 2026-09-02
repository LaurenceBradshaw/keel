#pragma once
#include <vector>
#include "ast/ast.h"
#include "ast/node.h"
#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/literals.h"
#include "common/source_manager.h"
#include "sema/resolver.h"
#include "sema/type.h"

namespace keel
{

class Types
{
public:
    Types() = default;
    Types( Type_table table, std::vector<Type_id> types )
        : table_( std::move( table ) ),
          types_( std::move( types ) )
    {
    }

    // Invalid when the node was never typed - a statement, a type annotation, or an error subtree.
    Type_id type_of( Node_id node ) const
    {
        return node.v < types_.size() ? types_[node.v] : Type_id {};
    }

    const Type_table& table() const
    {
        return table_;
    }

private:
    Type_table           table_;
    std::vector<Type_id> types_;
};

Types type_check( const Ast&, const Resolution&, const Literals&, const Source_manager&, const Interner&, Diagnostics& );

} // namespace keel