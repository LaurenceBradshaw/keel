#pragma once
#include <optional>
#include <unordered_map>
#include "ast/ast.h"
#include "common/literal_pool.h"
#include "common/types.h"
#include "sema/bounds.h"
#include "sema/reporter.h"
#include "sema/type_checker.h"
#include "sema/types_builder.h"

namespace keel
{
namespace sema
{

// A folded constant, in the sign-magnitude pair fits() already takes. Arithmetic wraps at run time
// (§12); wrapping it here instead would be a wrong answer delivered in silence.
//
// `overflowed` is not the same as not being constant: `18446744073709551615 * 2` is constant and
// has left every Keel type there is, and losing that distinction lets the largest constants through.
struct Constant
{
    u64  magnitude = 0;
    bool negative  = false;
};

struct Folded
{
    bool     constant   = false;
    bool     overflowed = false;
    Constant value;
};

class Constant_folder
{
public:
    Constant_folder( const Ast& ast, const Literal_pool& literals, Types_builder& types, Bounds& bounds, Reporter& reporter )
        : ast_( ast ),
          literal_pool_( literals ),
          types_( types ),
          table_( types.table() ),
          bounds_( bounds ),
          reporter_( reporter )
    {
    }

    // What may initialise a file-scope variable: what C also accepts as a constant expression.
    bool is_constant_expression( Node_id id ) const;

    // Constant rejection (§12). fold_* answer what an expression's value is when it is made only
    // of literals; check_constant decides whether that value can exist in the type the operation
    // happens in, and record_constant is the two together.
    // `known` is the type the caller already settled on - see check_constant for why it is needed.
    Folded             fold_integer( Node_id id, Type_id known = Type_id {} ) const;
    std::optional<f64> fold_float( Node_id id ) const;

    bool    check_constant( Node_id id, Type_id type );
    Type_id record_constant( Node_id id, Type_id type );

    void                                    record_value( Node_id id, Constant_value value );
    std::optional<Constant_value>           value_of( Node_id id ) const;
    std::unordered_map<u32, Constant_value> take_values();

private:
    const Ast&          ast_;
    const Literal_pool& literal_pool_;
    Types_builder&      types_;
    const Type_table&   table_;
    Bounds&             bounds_;
    Reporter&           reporter_;

    std::unordered_map<u32, Constant_value> constants_;
};
} // namespace sema
} // namespace keel
