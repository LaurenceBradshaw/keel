// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <vector>
#include "ast/ast.h"
#include "ast/node.h"
#include "common/interner.h"
#include "lex/token.h"
#include "sema/reporter.h"
#include "sema/type.h"

// Which method answers each of an interface's, for every `implements` written. Read by the body
// checks, by lowering for a call through a bound, and by the emitter for each vtable.

namespace keel
{

// One `implements`: the implementer's method for each of the interface's, in the interface's order.
struct Conformance
{
    Node_id              implementer {}; // the aggregate or the block whose methods answer
    Type_id              type {};        // what conforms: an aggregate's open form, or a block's type
    Type_id              interface {};
    std::vector<Node_id> requirements {}; // the interface's methods, in declaration order
    std::vector<Node_id> methods {};      // parallel to requirements; invalid where none answers
};

enum class Conformance_problem : u8
{
    Missing,     // no method of the name
    Mismatched,  // one of the name, with other parameters or another return
    Not_const,   // the interface's is `const` and the implementer's is not
    Static,      // the implementer's takes no object
    Not_static,  // the requirement takes no object, the implementer's does
    Generic,     // the implementer's has type parameters of its own
    Private,     // the implementer's is hidden from a caller holding the interface
    Repeated,    // the type already conforms to the interface
    Unrequested, // a block's method the interface does not declare
    Unrefined,   // the type does not conform to an interface this one requires
};

struct Conformance_error
{
    Conformance_problem problem = Conformance_problem::Missing;
    Span                at {};          // the interface as written; an unrequested method's name
    Node_id             implementer {}; // the aggregate or the block
    Type_id             interface {};
    Node_id             requirement {}; // the interface's method
    Node_id             found {};   // the implementer's method of that name, where one exists; Repeated: the first implementer
    Type_id             self {};    // the implementer's type, which the requirement's `self` stands for
    Type_id             refined {}; // Unrefined: the interface required
};

struct Conformances
{
    std::vector<Conformance>       conformances;
    std::vector<Conformance_error> errors;

    const Conformance* find( Node_id implementer, Type_id interface ) const;
    // By the type that conforms, so `Box<i32>` finds `Box<T>`'s.
    const Conformance* of( const Type_table& table, Type_id type, Type_id interface ) const;
    Node_id            method_for( const Type_table& table, Type_id type, Type_id interface, Node_id requirement )
        const; // invalid where none answers
};

// Asked of each aggregate's `implements` list, and of each block, once every signature is typed.
Conformances check_conformances( const Ast& ast, Type_table& table, std::span<const Type_id> recorded );

// The prelude interface an operator asks for: `equatable` for `==` and `!=`, `comparable` for the
// orderings, and invalid otherwise.
Type_id interface_for( const Type_table& table, Token_kind op );
// What the compiler makes a type of this kind, whatever the type declares.
bool supplied_by_kind( const Ast& ast, const Type_table& table, Type_id type, Type_id interface );
// supplied_by_kind, unless the type's own `implements` answers for it or for `equatable`.
bool supplies( const Ast& ast, const Type_table& table, Type_id type, Type_id interface );

} // namespace keel

namespace keel::sema
{

// One error per problem, each at the interface as written after `implements`.
void report_conformance_errors(
    const Ast&                         ast,
    const Interner&                    interner,
    Type_table&                        table,
    std::span<const Type_id>           recorded,
    std::span<const Conformance_error> errors,
    Reporter&                          reporter
);

} // namespace keel::sema
