// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ast/ast.h"
#include "ast/node.h"
#include "common/interner.h"
#include "sema/reporter.h"
#include "sema/types_builder.h"

// Structs, classes and enums as *shapes*: what contains what, and where a member or a field is.
// No rule about them lives here - only the answers the rules ask for.

namespace keel::sema
{

// Owns the containment order, the one fact about an aggregate computed once for the whole program
// rather than at a use site.
class Aggregates
{
public:
    Aggregates( const Ast& ast, const Interner& interner, Types_builder& types, Reporter& reporter )
        : ast_( ast ),
          interner_( interner ),
          types_( types ),
          table_( types.table() ),
          reporter_( reporter )
    {
    }

    // The DFS post-order over by-value containment, reporting any cycle it finds. Returns the
    // aggregates that were on one, so the caller can keep its own rules quiet about them.
    std::vector<Node_id> order_structs();

    // Whether order_structs proved this declaration, and everything it holds by value, free of a
    // containment cycle. False before order_structs has run.
    bool is_acyclic( Node_id decl ) const;
    bool is_ordered() const
    {
        return ordered_;
    }

    // Whether any struct named in this type, its arguments included, is on a reported cycle.
    bool mentions_a_cycle( Type_id type ) const;

    // The first member of a kind, or invalid. Constructors and destructors are both at most one,
    // so "the first" and "the only" coincide once check_aggregate_members has run.
    Node_id find_member( Node_id decl, Node_kind kind ) const;
    Node_id find_method( Node_id decl, Symbol_id name ) const;
    Node_id find_field( Type_id type, Symbol_id name ) const;
    // `C::C`: the name a class's constructor was declared with, which only `C( ... )` reaches.
    bool names_constructor( Node_id aggregate, Symbol_id name ) const;
    // The operator `name` taking `operands` right-hand operands, or invalid; `malformed` when one of
    // another count was declared, which was reported where it was written.
    Node_id find_operator( Node_id decl, Symbol_id name, std::size_t operands, bool& malformed ) const;

    void report_private( Node_id at, Node_id member ) const;

    // What binds an aggregate's type parameters to what it was instantiated at. Empty for a
    // non-generic one, and the identity for the open form, so callers need no branch.
    Bindings bindings_of( Type_id aggregate ) const;

    // bindings_of, plus an interface method's `self` as the receiver, so under `where T : P` a
    // requirement's `self` is `T`, and the parameters of `interface`, the instance it is reached
    // through. `interface` is invalid for a receiver that is not a parameter.
    Bindings receiver_bindings( Type_id receiver, Node_id method, Type_id interface = {} ) const;

    // A field's type *as seen through this instance*. The declared type of `Box<T>`'s field is `T`,
    // which is true of the template and of no value anyone holds - so every read of a field's type
    // goes through here rather than through the recorded type of the field declaration.
    Type_id field_type( Type_id aggregate, Node_id field );

    std::vector<Node_id> carried_payload( Type_id instance, Node_id variant );

    // Handing the run's result to Types. Leaves this object empty; nothing reads it after.
    std::vector<Node_id> take_struct_order();

private:
    void report_containment_cycle( Node_id decl, const std::vector<Node_id>& path );
    bool contains_itself( Node_id decl, std::vector<Node_id>& path );
    bool contains_through( Type_id type, std::vector<Node_id>& path );
    bool holds_by_value( Node_id generic, std::size_t index, std::vector<Node_id>& visiting ) const;
    bool mentions_by_value( Type_id type, Type_id parameter, std::vector<Node_id>& visiting ) const;

    const Ast&      ast_;
    const Interner& interner_;
    Types_builder&  types_;
    Type_table&     table_;
    Reporter&       reporter_;

    std::vector<Node_id> struct_order_;      // dependencies first
    std::vector<Node_id> containment_order_; // every aggregate, generics too, dependencies first; the proved-acyclic set
    bool                 ordered_ = false;
};

} // namespace keel::sema
