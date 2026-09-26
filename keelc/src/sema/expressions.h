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

namespace keel
{
namespace sema
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
    void absorb( Node_id id );

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
    // `&f`: the operand names a function, so the type is its signature and not a pointer to it.
    Type_id function_address( Node_id id, Node_id declaration );

    // The function type a declaration would be written as; refused receives the first parameter carrying a mode.
    Type_id written_signature( Node_id declaration, const Bindings& bindings, Node_id& refused );
    // Which overload the expected signature names, or nothing - reported - when the set holds no such one.
    Node_id overload_for_signature( Node_id id, std::string_view name, Node_id first, Type_id signature );
    Type_id indirect_call( Node_id id, Node_id declaration, Type_id signature );

    Type_id infer_field( Node_id id );

    // A composite constructor, not a value literal: its type is fixed by its name rather than
    // adopted from context, which is why it has no place in Literals::infer_literal.
    Type_id infer_struct_literal( Node_id id );
    Type_id no_instance_named( Node_id id, Node_id declaration );

    Type_id infer_cast( Node_id id ); // D35's unsafe gate is here rather than in Operators::convert
    Type_id infer_marker( Node_id id );

    Type_id infer_path( Node_id id );
    Type_id infer_variant_construction( Node_id id );

    // PLAN §12, M7. `P::make( 7 )` - a member reached through its type rather than an object. The
    // declaration a path's qualifier names, and the type it stands for once its own type arguments
    // are applied, which for a static call is the only place those can come from.
    Node_id qualifier_declaration( Node_id path ) const;

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

    // D35: an operation on the enumerated list is permitted inside an unsafe block and reported
    // outside one. The `why` is the operation's own reason - what the author is asserting by writing
    // the block - so each caller supplies it.
    void require_unsafe( Node_id id, std::string what, std::string why = {} );

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
    // by a variant path and a struct literal to say which instance they name, and by a call to
    // deduce the type arguments nobody wrote. A consumer takes it and clears it, so nothing nested
    // inside the expression reads an expectation that belongs to its parent.
    Type_id expected_;
    Node_id current_function_; // whose annotation the escape rule reads

    u32  unsafe_depth_ = 0; // an unsafe operation is permitted while this is non-zero
    bool unsafe_used_  = false;
};

} // namespace sema
} // namespace keel
