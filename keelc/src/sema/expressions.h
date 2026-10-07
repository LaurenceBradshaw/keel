// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <string>
#include "ast/ast.h"
#include "common/interner.h"
#include "sema/aggregates.h"
#include "sema/annotations.h"
#include "sema/bounds.h"
#include "sema/callees.h"
#include "sema/constant_folder.h"
#include "sema/generic_recursion.h"
#include "sema/literals.h"
#include "sema/operators.h"
#include "sema/overloads.h"
#include "sema/places.h"
#include "sema/reporter.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"
#include "sema/types_builder.h"

namespace keel::sema
{

// The expression walk: `infer` and `check` and one member per node kind. Every other class in
// sema/ asks whether it can avoid re-entering this; this one *is* it, so the rule is checked in
// the mirror - expressions never contain statements, which is what puts `Statements` above this
// class rather than beside it. Grep expressions.cpp for `visit(` and expect nothing.
//
// It names every class above it, which is what being the top of the rule stack means.
class Expressions
{
public:
    Expressions(
        const Ast&         ast,
        const Interner&    interner,
        const Resolution&  resolution,
        Types_builder&     types,
        Aggregates&        aggregates,
        Bounds&            bounds,
        Annotations&       annotations,
        Constant_folder&   constant_folder,
        Callees&           callees,
        Places&            places,
        Generic_recursion& generic_recursion,
        Literals&          literals,
        Operators&         operators,
        Overloads&         overloads,
        Reporter&          reporter
    )
        : ast_( ast ),
          interner_( interner ),
          resolution_( resolution ),
          types_( types ),
          table_( types.table() ),
          aggregates_( aggregates ),
          bounds_( bounds ),
          annotations_( annotations ),
          constant_folder_( constant_folder ),
          callees_( callees ),
          places_( places ),
          generic_recursion_( generic_recursion ),
          literals_( literals ),
          operators_( operators ),
          overloads_( overloads ),
          reporter_( reporter )
    {
    }

    // Every branch of both ends in a `types_.record`, including the error ones: a node the walk
    // reached and left untyped is what later passes assert on rather than report.
    Type_id infer( Node_id id );                   // expression, no expectation
    Type_id check( Node_id id, Type_id expected ); // expression, with one

    // Walks an expression only for the errors inside it, in a context that has already failed.
    void    absorb( Node_id id );
    Type_id take_expectation(); // expected_, cleared

    // D5: this expression has to be a `bool` already. A rule about one expression rather than about
    // the statement around it, which is why `if`, `while`, `for` and the ternary all reach it here.
    void check_condition( Node_id id );

    // A variant name in a pattern position - a `switch` arm's path, or a bare `case` label. The
    // payload is accounted for there, so `Shape::Circle` is complete; `Shape s = Shape::Circle;`
    // still is not. One entry point rather than two fields a caller sets, because the pair of them
    // is this walk's state and the two callers only ever set and clear them together.
    Type_id infer_in_pattern( Node_id path, Type_id scrutinee );

    // Whose body the walk is inside. Written only around a function, read by four classes: the two
    // below take it as a parameter, and `Statements` above reads it back. It is walk state, and
    // this is the walk - which is what settled a field that looked as though it belonged to no one.
    Node_id enter_function( Node_id id ); // returns the enclosing one, for leave_function
    void    leave_function( Node_id enclosing );
    Node_id current_function() const
    {
        return current_function_;
    }

    // D35's block. The caller visits the children between these two - they are statements, which is
    // why the block cannot move here - and both of the block's own diagnostics are statements about
    // the flag, so they belong to whoever owns it.
    bool enter_unsafe( Span at ); // returns the enclosing `used` flag, for leave_unsafe
    void leave_unsafe( Span at, bool enclosing_used );

    void step_pointer( Node_id statement, Token_kind op, Type_id pointer, Node_id value );

private:
    Type_id infer_name( Node_id id );
    Type_id infer_call( Node_id id );

    // What an argument can say about itself before a candidate has been chosen. It takes a node and
    // infers it, which is the half `Overloads` may not do - so the walk keeps it.
    Argument_shape argument_shape( Node_id argument );

    Type_id infer_method_call( Node_id id );
    Type_id infer_implicit_method_call( Node_id id, Node_id method );
    Type_id check_method_arguments( Node_id id, Node_id method, Type_id receiver, std::span<const Argument_shape> shapes = {} );
    void    record_method_instantiation( Node_id id, Node_id method, Type_id receiver );

    // These two infer their operands and hand the types to `Operators`, which holds the rules.
    // Address-of and dereference stay in infer_unary: neither is in the rule table, and the first
    // is a question about places rather than types.
    Type_id infer_binary( Node_id id );
    Type_id infer_unary( Node_id id );
    Type_id infer_operator_call( Node_id id, Node_id object, Node_id argument, Symbol_id name );
    Type_id infer_index_operator( Node_id id, Type_id object_type );
    Node_id
    find_operator( Node_id id, Type_id object_type, Node_id argument, Symbol_id name, std::string_view declaration_text );
    // `&f`: the operand names a function, so the type is its signature and not a pointer to it.
    Type_id function_address( Node_id id, Node_id declaration );
    // `&C::f`: an instance method's signature, with its receiver as parameter 0.
    Type_id method_address( Node_id id, Node_id aggregate );
    // `C::C`: the name a class's constructor was declared with, which only `C( ... )` reaches.
    bool names_constructor( Node_id aggregate, Symbol_id name ) const;

    // `&C::x`: the offset of a field, typed `field( C ) -> T` at the qualifier's instance.
    Type_id field_address( Node_id id, Node_id aggregate, Node_id field );
    // `p( obj )`, where `p` holds an offset: a copy of that field of `obj`.
    Type_id field_application( Node_id id, Type_id offset );

    // The function type a declaration would be written as; refused receives the first parameter carrying a mode.
    Type_id written_signature( Node_id declaration, const Bindings& bindings, Node_id& refused );
    // Which overload the expected signature names, or nothing - reported - when the set holds no such one.
    Node_id
    overload_for_signature( Node_id id, std::string_view name, Node_id first, Type_id signature, const Bindings& bindings );
    Type_id indirect_call( Node_id id, Node_id declaration, Type_id signature );

    Type_id infer_field( Node_id id );
    Type_id infer_string_literal( Node_id id );

    // A composite constructor, not a value literal: its type is fixed by its name rather than
    // adopted from context, which is why it has no place in Literals::infer_literal.
    Type_id infer_struct_literal( Node_id id );
    Type_id no_instance_named( Span at, Node_id declaration, Type_id expectation );

    Type_id infer_cast( Node_id id ); // D35's unsafe gate is here rather than in Operators::convert
    Type_id infer_marker( Node_id id );

    Type_id infer_path( Node_id id );
    Type_id infer_variant_construction( Node_id id );

    // PLAN §6.7. `P::make( 7 )` - a member reached through its type rather than an object. The
    // declaration a path's qualifier names, and the type it stands for once its own type arguments
    // are applied, which for a static call is the only place those can come from.
    Node_id qualifier_declaration( Node_id path ) const;
    // A bare name, or one written through its package: the two things a use binds.
    bool is_name( Node_id id ) const;

    // The type the access being checked is written inside, or an invalid id in a free function.
    // Asked of the enclosing *function*, never of a receiver: a static method has no receiver and
    // is as much inside its type as an instance method is.
    Node_id current_type() const;

    // The shared half of the five refusals, which differ only in what they return afterwards.
    void    report_private( Node_id at, Node_id member );
    Type_id qualifier_type( Node_id path, Node_id declaration );
    Type_id infer_static_call( Node_id id, Node_id aggregate );

    Type_id infer_conditional( Node_id id );

    Type_id infer_alloc( Node_id id );
    Type_id infer_free( Node_id id );
    Type_id infer_destroy( Node_id id );

    Type_id infer_index( Node_id id );

    // D35: an operation on the enumerated list is permitted inside an unsafe block and reported
    // outside one. The `why` is the operation's own reason - what the author is asserting by writing
    // the block - so each caller supplies it.
    void require_unsafe( Node_id id, std::string what, std::string why = {} );

    bool refuses_many_member( Node_id field_expr, Type_id base_type );

    Node_id next_visible( Node_id use, Node_id candidate ) const;

    // A literal, or arithmetic made only of them: what check() may give the expected type to.
    bool takes_context( Node_id id ) const;

    const Ast&         ast_;
    const Interner&    interner_;
    const Resolution&  resolution_;
    Types_builder&     types_;
    Type_table&        table_;
    Aggregates&        aggregates_;
    Bounds&            bounds_;
    Annotations&       annotations_;
    Constant_folder&   constant_folder_;
    Callees&           callees_;
    Places&            places_;
    Generic_recursion& generic_recursion_;
    Literals&          literals_;
    Operators&         operators_;
    Overloads&         overloads_;
    Reporter&          reporter_;

    // Whether a bare variant path here names the variant rather than standing for a value of
    // the enum. True while checking a construction's callee, a pattern's path, and a bare `case`
    // label - in all three the payload is accounted for, so `Shape::Circle` is complete.
    bool naming_variant_ = false;

    // What the surrounding context wants this expression to be. Set by check() around the
    // expectation it is testing, and by the two places a pattern already knows the scrutinee. Read
    // only through take_expectation, by a variant path and a struct literal to say which instance
    // they name, by a call to deduce the type arguments nobody wrote, and by `&f` to choose an
    // overload - so nothing nested inside reads an expectation that belongs to its parent.
    Type_id expected_;
    Node_id current_function_; // whose annotation the escape rule reads

    u32  unsafe_depth_ = 0; // an unsafe operation is permitted while this is non-zero
    bool unsafe_used_  = false;
};

} // namespace keel::sema
