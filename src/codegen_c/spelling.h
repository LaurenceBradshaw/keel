#pragma once
#include <string>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/literals.h"
#include "common/types.h"
#include "sema/type_checker.h"

namespace keel
{
struct Spelling
{
    const Ast&      ast;
    const Types&    types;
    const Literals& literals;
    const Interner& interner;

    std::string type( Type_id type ) const; // "int32_t", "struct kl__Point", "int32_t*"
    std::string structure( Node_id declaration ) const;
    std::string field( Node_id declaration ) const;
    std::string function( Node_id declaration ) const;
    // The types alone: `int32_t, uint8_t`. That is all a prototype needs, and all a definition can
    // use - the body names its parameters by Local_id, so nothing here may name them at all.
    std::string parameter_types( Node_id declaration ) const;

    // A file-scope variable's whole definition. Its initialiser stays an expression rather than a
    // value: a C file-scope initialiser must be one constant expression, and there is nowhere at
    // file scope to put the temporaries three-address form would need.
    std::string global_definition( Node_id declaration ) const;

private:
    std::string constant_expression( Node_id node ) const;

public:
};

// The two rules both constant spellers need. The dispatch above them differs - a global's
// initialiser is still an AST expression and switches on the node kind, while a KIR operand
// carries a Literal_id and a type and switches on the type - but these do not.

// fmt's default is the shortest form that round-trips, and a value like 1.0 prints as "1", which C
// would read as an int.
std::string c_float( f64 value );

// The suffix stops C choosing a signed type too narrow to hold the value. Deliberately *not* a
// cast to the literal's own type: `-2147483648` is a negation of 2147483648, and casting that
// first would overflow before the minus ran. The temporary each operation writes into carries the
// explicit C type instead.
std::string c_integer( u64 value );
} // namespace keel