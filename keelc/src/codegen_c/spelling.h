#pragma once
#include <span>
#include <string>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/types.h"
#include "sema/type_checker.h"

namespace keel
{
struct Spelling
{
    const Ast&      ast;
    const Types&    types;
    const Interner& interner;

    std::string type( Type_id type ) const; // "int32_t", "struct kl__Point", "int32_t*"
    std::string structure( Node_id declaration ) const;
    std::string field( Node_id declaration ) const;
    // The declaration alone no longer names a function: one generic is emitted once per set of
    // type arguments, so they are part of the symbol.
    std::string function( Node_id declaration, std::span<const Type_id> type_arguments = {} ) const;

    // The symbol a Drop calls. A Drop always names a type with a destructor of its own - the
    // lowerer expands a compound into per-field drops - so a type without one is a lowering bug
    // rather than a case to handle.
    std::string destructor_of( Type_id type ) const;
    // The types alone: `int32_t, uint8_t`. That is all a prototype needs, and all a definition can
    // use - the body names its parameters by Local_id, so nothing here may name them at all.
    std::string parameter_types( Node_id declaration ) const;

    // What C sees a function hand back. Not the declared type when the function returns a binding:
    // that travels as an address, so the definition, the prototype and the KIR return slot must all
    // say the pointer or C is told one thing and given another.
    std::string return_type( Node_id declaration ) const;

    // A file-scope variable's whole definition. Its initialiser stays an expression rather than a
    // value: a C file-scope initialiser must be one constant expression, and there is nowhere at
    // file scope to put the temporaries three-address form would need.
    std::string global_definition( Node_id declaration ) const;
};

// How C is told what kind of number it is looking at. Used by the KIR emitter for a constant
// operand, and by global_definition for a folded initialiser.

// fmt's default is the shortest form that round-trips, and a value like 1.0 prints as "1", which C
// would read as an int.
std::string c_float( f64 value );

// The suffix stops C choosing a signed type too narrow to hold the value. Deliberately *not* a
// cast to the literal's own type: `-2147483648` is a negation of 2147483648, and casting that
// first would overflow before the minus ran. The temporary each operation writes into carries the
// explicit C type instead.
std::string c_integer( u64 value );
} // namespace keel