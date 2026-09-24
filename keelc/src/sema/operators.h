#pragma once
#include "ast/ast.h"
#include "common/interner.h"
#include "lex/token.h"
#include "sema/bounds.h"
#include "sema/reporter.h"
#include "sema/type_checker.h"

namespace keel
{
namespace sema
{

// Where a binary operator's result type comes from. The rule table itself is private; this is the
// one question a caller has to ask of it, to know which operands an expectation may flow into.
enum class Result_source : u8
{
    None,         // a comparison, or an operator with no rule - an expectation reaches neither operand
    Operands,     // the §6.4 result of the two, so both take it
    Left_operand, // a shift's result is its left operand's, so only that one does
};

// What the caller must still do with a binary result the rules accepted.
struct Binary_result
{
    Type_id type; // invalid: refused, and the diagnostic is already written

    // Whether this landed on the one path D41's warning is owed on, which the caller cannot
    // reconstruct: the two branches above it may return without ever reaching the rule table.
    bool compare_constant = false;
};

// The same, for a conversion.
struct Conversion_result
{
    Type_id type; // invalid: refused, and the diagnostic is already written

    // D35's gate is the caller's, because the depth it counts is the expression walk's state.
    bool needs_unsafe = false;
};

// What an operator answers, given operand types the caller has already inferred. A service takes
// types and spans rather than nodes, so none of these can wander off and check one more thing.
// `ast_` is here for one help line and is never walked.
class Operators
{
public:
    Operators( const Ast& ast, const Interner& interner, const Type_table& table, const Bounds& bounds, Reporter& reporter )
        : ast_( ast ),
          interner_( interner ),
          table_( table ),
          bounds_( bounds ),
          reporter_( reporter )
    {
    }

    Result_source result_source( Token_kind op ) const;

    // Binary. D11's generic branch, pointer and enum identity, then the rule table: operand
    // classes, D41's bool answer for a comparison, a shift's left operand, and §6.4's common type.
    // The node is the enclosing function, and only its name is read - a `where` clause has to be
    // written somewhere, and the help says where.
    Binary_result result_of_binary( Token_kind op, Type_id lhs, Type_id rhs, Node_id current_function, Span at );

    // The four operators the rule table holds - `-`, `+`, `!`, `~`. Address-of and dereference are
    // not among them: both change the result type rather than keeping the operand's, and the first
    // is a question about places rather than about types.
    Type_id result_of_unary( Token_kind op, Type_id operand, Span at );

    // The ternary, once both arms are typed. They must simply agree.
    Type_id result_of_conditional( Type_id then_type, Type_id else_type, Span at );

    // `cast` and `wrap` over a resolved target: which conversions exist, and the help for the ones
    // that do not.
    Conversion_result convert( bool is_cast, Type_id value, Type_id target, Span at );

private:
    const Ast&        ast_;
    const Interner&   interner_;
    const Type_table& table_;
    const Bounds&     bounds_;
    Reporter&         reporter_;
};

} // namespace sema
} // namespace keel
