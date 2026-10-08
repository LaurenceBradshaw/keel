// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <string_view>
#include <type_traits>
#include "common/span.h"
#include "common/types.h"

namespace keel
{

enum class Node_kind : u16
{
    // A rule that failed. Returned instead of an invalid Node_id so the tree stays well formed and
    // arity stays fixed; later passes skip Error subtrees silently. aux: the declaration's name when
    // one was read, else invalid.
    Error,

    // children: the declarations.
    Source_file,
    // aux: the module; children: the package Name_expr, when written.
    Import_decl,
    // aux: the name; children: return type, Param_list, body Block (invalid for `extern`),
    // Type_param_list (invalid when not generic).
    Function_decl,
    // aux: the name; children as Function_decl, with no return type and the aggregate's type parameters.
    Destructor_decl,
    // aux: the name; children as Destructor_decl.
    Constructor_decl,
    // aux: the name (`this` for a receiver, invalid in a Function_type); children: the type.
    Param_decl,
    // aux: the name; children: the package Name_expr, when written.
    Named_type,
    // children: the pointee.
    Pointer_type,
    // `T[*]`; children: the element.
    Many_pointer_type,
    // aux: the Keyword (`ref`, `out`, `move`); children: the type.
    Mode_type,
    // children: the type.
    Const_type,
    // children: the Named_type, then its Type_arg_list.
    Generic_type,
    // children: return type, then a Param_list of unnamed Param_decls.
    Function_type,
    // `field( A ) -> T`; children: the field's type, then the aggregate's.
    Field_type,
    // children: the types.
    Type_arg_list,
    // children: the Param_decls, the receiver first when there is one.
    Param_list,
    // aux: 1 for `unsafe`; children: the statements.
    Block,
    // children: the value (invalid when absent).
    Return_stmt,
    // aux: the Literal_id.
    Int_literal,
    // aux: the Literal_id.
    Float_literal,
    // aux: the Literal_id.
    String_literal,
    // aux: the Literal_id.
    Char_literal,
    // aux: 1 for `true`.
    Bool_literal,
    // aux: the name (`this` for the receiver); children: its Type_arg_list, when written uncalled.
    Name_expr,
    // aux: the operator's Token_kind; children: left, then right.
    Binary_expr,
    // aux: the operator's Token_kind; children: the operand.
    Unary_expr,
    // children: condition, then the two arms.
    Conditional_expr,
    // children: callee, Arg_list, Type_arg_list (invalid when not written).
    Call_expr,
    // children: the arguments.
    Arg_list,
    // A local, a global or a static field. aux: the name; children: the annotation (invalid for
    // `auto`), then the initialiser (invalid when absent).
    Var_decl,
    // aux: the operator's Token_kind; children: target, then value.
    Assign_stmt,
    // aux: `++` or `--` as a Token_kind; children: the operand.
    Increment_stmt,
    // children: the expression.
    Expr_stmt,
    // children: condition, then Block, else (a Block or an If_stmt; invalid when absent).
    If_stmt,
    // children: condition, then body.
    While_stmt,
    // children: init (a Var_decl or a statement), condition, update, body; the first three invalid
    // when absent.
    For_stmt,
    // aux: the name; children: the Type_param_list (invalid when not generic), then the members:
    // Field_decls, static Var_decls, Method_decls, Constructor_decls and the Destructor_decl.
    Struct_decl,
    // As Struct_decl.
    Class_decl,
    // aux: the name; children: Type_param_list, underlying type (each invalid when absent), then the
    // Variant_decls.
    Enum_decl,
    // aux: the name; children: the payload's Field_decls.
    Variant_decl,
    // `Colour::Red`; aux: the member's name; children: the qualifier, then its Type_arg_list when
    // written.
    Path_expr,
    // aux: the name; children: the type.
    Field_decl,
    // aux: the field's name; children: the object.
    Field_expr,
    // `p[i]`; children: the object, then the index.
    Index_expr,
    // aux: the type's name; children: the package Name_expr when written, then the Field_inits.
    Struct_literal,
    // aux: the field's name (invalid when positional); children: the value.
    Field_init,
    // `move`, `out`, `ref`: an operator that does not change its operand's type. aux: the Keyword;
    // children: the operand.
    Marker_expr,
    Null_literal,
    // aux: the Keyword (`cast` or `wrap`); children: the type, then the operand.
    Cast_expr,
    Break_stmt,
    Continue_stmt,
    // children: the scrutinee, then the Case_arms.
    Switch_stmt,
    // aux: 1 when `default` labels it; children: the labels (expressions, Range_exprs and
    // Variant_patterns), then the body Block.
    Case_arm,
    // `1..5`, half-open; children: the two bounds.
    Range_expr,
    // `Shape::Circle( r )`; children: the Path_expr, then the Binding_decls.
    Variant_pattern,
    // A name bound by a pattern. aux: the name.
    Binding_decl,
    // aux: the name; children as Function_decl, with the enclosing aggregate's type parameters.
    Method_decl,
    // `alloc<T>()` or `alloc<T>( n )`; children: the type, then the count when written.
    Alloc_expr,
    // `free( p )`; children: the pointer.
    Free_expr,
    // `assert( c )`; children: the condition.
    Assert_expr,
    // `destroy( p, n )`; children: the pointer, then the count.
    Destroy_expr,
    Fallthrough_stmt,
    // children: the Type_param_decls, then the Where_clauses constraining them. One node, so a
    // function-like declaration keeps four children and the resolver declares the parameters
    // before anything names one.
    Type_param_list,
    // aux: the name. Its bounds are in a Where_clause beside it.
    Type_param_decl,
    // aux: the parameter it constrains; children: the Bound_names.
    Where_clause,
    // aux: the bound's name, resolved against D40's fixed set by the checker.
    Bound_name,
    // TODO: write this comment.
    Try_expr,

    Count
};

inline constexpr u32 k_invalid_node = 0xFFFF'FFFFu;

struct Node_id
{
    u32  v = k_invalid_node;
    bool is_valid() const
    {
        return v != k_invalid_node;
    }
    auto operator<=>( const Node_id& other ) const = default;
};

// The enum spelling, for --dump-ast and debugging. A switch with no default, so -Wswitch turns a
// forgotten kind into a release build failure.
std::string_view node_kind_name( Node_kind kind );

// D29: a struct and a class differ in rules, not shape. Field layout, containment ordering and C
// emission treat them identically; only the rule checks read the kind directly.
constexpr bool is_aggregate( Node_kind kind )
{
    return kind == Node_kind::Struct_decl || kind == Node_kind::Class_decl;
}

// A destructor is a Function_decl minus its return type, so one scan finds both.
constexpr bool is_function_like( Node_kind kind )
{
    return kind == Node_kind::Function_decl || kind == Node_kind::Destructor_decl || kind == Node_kind::Constructor_decl ||
           kind == Node_kind::Method_decl;
}

struct Node
{
    Node_kind kind;        // u16
    Span      span;        // 12 bytes
    u32       aux;         // meaning depends on kind
    u32       first_child; // offset into children_
    u32       child_count;
}; // 28 bytes

static_assert( sizeof( Node ) == 28, "Node must stay compact" );
static_assert( std::is_trivially_copyable_v<Node> );

} // namespace keel
