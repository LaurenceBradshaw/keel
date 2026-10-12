// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <vector>
#include "ast/ast.h"
#include "ast/node.h"
#include "common/interner.h"
#include "sema/reporter.h"
#include "sema/type.h"

// Which method answers each of an interface's, for every `implements` written. Read by the body
// checks, by lowering for a call through a bound, and by the emitter for each vtable.

namespace keel
{

// One `implements`: the implementer's method for each of the interface's, in the interface's order.
struct Conformance
{
    Node_id              implementer {}; // the aggregate's declaration
    Type_id              interface {};
    std::vector<Node_id> requirements {}; // the interface's methods, in declaration order
    std::vector<Node_id> methods {};      // parallel to requirements; invalid where none answers
};

enum class Conformance_problem : u8
{
    Missing,    // no method of the name
    Mismatched, // one of the name, with other parameters or another return
    Not_const,  // the interface's is `const` and the implementer's is not
    Static,     // the implementer's takes no object
    Generic,    // the implementer's has type parameters of its own
    Private,    // the implementer's is hidden from a caller holding the interface
    Repeated    // the interface is named twice in one `implements`
};

struct Conformance_error
{
    Conformance_problem problem = Conformance_problem::Missing;
    Span                at {};          // the interface as written after `implements`
    Node_id             implementer {}; // the aggregate
    Type_id             interface {};
    Node_id             requirement {}; // the interface's method
    Node_id             found {};       // the implementer's method of that name, where one exists
    Type_id             self {};        // the implementer's type, which the requirement's `self` stands for
};

struct Conformances
{
    std::vector<Conformance>       conformances;
    std::vector<Conformance_error> errors;

    const Conformance* find( Node_id implementer, Type_id interface ) const;
    Node_id            method_for( Node_id implementer, Node_id requirement ) const; // invalid where none answers
};

// Asked of each aggregate's `implements` list once every signature is typed.
Conformances check_conformances( const Ast& ast, Type_table& table, std::span<const Type_id> recorded );

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
