#pragma once
#include <optional>
#include <unordered_map>
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

// A file-scope initialiser, evaluated. Two kinds cover everything Keel allows there: a bool is the
// integer 0 or 1, and `nullptr` is the integer 0. Integers keep the sign-magnitude form fits()
// already takes, so the range checks and the folder agree without converting between them.
struct Constant_value
{
    enum class Kind : u8
    {
        Integer,
        Float
    };

    Kind kind      = Kind::Integer;
    u64  magnitude = 0;
    bool negative  = false;
    f64  floating  = 0.0;
};

class Types
{
public:
    Types() = default;
    Types(
        Type_table                              table,
        std::vector<Type_id>                    types,
        std::vector<Node_id>                    struct_order,
        std::unordered_map<u32, Constant_value> constants
    )
        : table_( std::move( table ) ),
          types_( std::move( types ) ),
          struct_order_( std::move( struct_order ) ),
          constants_( std::move( constants ) )
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

    // Set for every file-scope variable that has an initialiser, and for nothing else - the rule
    // requires one to be a constant expression, so failing to fold one is an internal error.
    std::optional<Constant_value> constant_of( Node_id node ) const
    {
        const auto found = constants_.find( node.v );

        return found == constants_.end() ? std::nullopt : std::optional<Constant_value>( found->second );
    }

    const std::vector<Node_id>& struct_order() const
    {
        return struct_order_;
    }

private:
    Type_table                              table_;
    std::vector<Type_id>                    types_;
    std::vector<Node_id>                    struct_order_;
    std::unordered_map<u32, Constant_value> constants_; // dependencies first, from the DFS post-order
};

Types type_check( const Ast&, const Resolution&, const Literals&, const Source_manager&, const Interner&, Diagnostics& );

} // namespace keel