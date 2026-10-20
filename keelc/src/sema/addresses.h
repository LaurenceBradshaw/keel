// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <string_view>
#include "ast/ast.h"
#include "common/interner.h"
#include "sema/aggregates.h"
#include "sema/annotations.h"
#include "sema/bounds.h"
#include "sema/callees.h"
#include "sema/overloads.h"
#include "sema/reporter.h"
#include "sema/types_builder.h"

namespace keel::sema
{

// `&f`, `&C::f` and `&C::x`: an overload set resolved against an expected signature. Below the walk,
// which hands over the two pieces of its state these read - the expectation and the enclosing function.
class Addresses
{
public:
    Addresses(
        const Ast&      ast,
        const Interner& interner,
        Types_builder&  types,
        Aggregates&     aggregates,
        Bounds&         bounds,
        Annotations&    annotations,
        Callees&        callees,
        Overloads&      overloads,
        Reporter&       reporter
    )
        : ast_( ast ),
          interner_( interner ),
          types_( types ),
          table_( types.table() ),
          aggregates_( aggregates ),
          bounds_( bounds ),
          annotations_( annotations ),
          callees_( callees ),
          overloads_( overloads ),
          reporter_( reporter )
    {
    }

    // `&f`: the operand names a function, so the type is its signature and not a pointer to it.
    // An extern's D35 gate is the caller's, which reads the callee recorded here.
    Type_id function_address( Node_id id, Node_id declaration, Type_id expectation, Node_id current_function );
    // `&C::f`: an instance method's signature, with its receiver as parameter 0; or `&C::x`.
    Type_id method_address( Node_id id, Node_id aggregate, Type_id expectation, Node_id current_function );
    // `@functions_of( T )`: a `T[N]` of the candidates whose signature is T's function field.
    Type_id functions_of( Node_id id, std::span<const Node_id> candidates );

private:
    // `&C::x`: the offset of a field, typed `field( C ) -> T` at the qualifier's instance.
    Type_id field_address( Node_id id, Node_id aggregate, Node_id field, Node_id current_function );

    // The function type a declaration would be written as; refused receives the first parameter carrying a mode.
    Type_id written_signature( Node_id declaration, const Bindings& bindings, Node_id& refused );
    // Which overload the expected signature names, or nothing - reported - when the set holds no such one.
    Node_id
    overload_for_signature( Node_id id, std::string_view name, Node_id first, Type_id signature, const Bindings& bindings );

    const Ast&      ast_;
    const Interner& interner_;
    Types_builder&  types_;
    Type_table&     table_;
    Aggregates&     aggregates_;
    Bounds&         bounds_;
    Annotations&    annotations_;
    Callees&        callees_;
    Overloads&      overloads_;
    Reporter&       reporter_;
};

} // namespace keel::sema
