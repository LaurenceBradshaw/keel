#pragma once
#include "ast/ast.h"
#include "common/literal_pool.h"
#include "common/types.h"
#include "lex/token.h"
#include "sema/bounds.h"
#include "sema/constant_folder.h"
#include "sema/reporter.h"
#include "sema/type_checker.h"
#include "sema/types_builder.h"

namespace keel
{
namespace sema
{

// Where a literal gets its type. D26: a literal has a value rather than a type until something
// tells it what to be, so these five answer one question and the class needs no state of its own.
class Literals
{
public:
    Literals(
        const Ast&             ast,
        const Literal_pool&    literals,
        Types_builder&         types,
        const Bounds&          bounds,
        const Constant_folder& constant_folder,
        Reporter&              reporter
    )
        : ast_( ast ),
          literals_( literals ),
          types_( types ),
          table_( types.table() ),
          bounds_( bounds ),
          constant_folder_( constant_folder ),
          reporter_( reporter )
    {
    }

    // Whether context can give this expression a type, rather than it having one of its own.
    bool is_literal_expression( Node_id id ) const;

    // The fallback defaults, for a literal with nothing to give it a type. `nullptr` has none.
    Type_id infer_literal( Node_id id );

    // The bidirectional half (L5): a literal adopts `expected` when its value fits, which is what
    // lets `u32 x = 42;` need no suffix. Also where a unary minus must be pushed through, so that
    // `i32 x = -2147483648;` range-checks as a negative rather than as an out-of-range positive.
    // Against a type parameter the question is instead whether it fits every type `T` may become.
    Type_id check_literal( Node_id id, Type_id expected );

    // D41: the type a literal takes when the operand beside it cannot hold its value. Empty when
    // it fits and should adopt as usual - which is every case but a comparison against a constant
    // the other side has no room for.
    Type_id standalone_literal_type( Node_id id, Type_id known ) const;

    // D41's other half: having made that comparison legal, say that its answer was never in doubt.
    void warn_if_constant_comparison( Node_id id, Token_kind op, Type_id lhs_type, Type_id rhs_type );

private:
    const Ast&             ast_;
    const Literal_pool&    literals_;
    Types_builder&         types_;
    const Type_table&      table_;
    const Bounds&          bounds_;
    const Constant_folder& constant_folder_;
    Reporter&              reporter_;
};

} // namespace sema
} // namespace keel
