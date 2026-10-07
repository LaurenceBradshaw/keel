// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <initializer_list>
#include <span>
#include <unordered_map>
#include <vector>
#include "ast/node.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "lex/token.h"

namespace keel
{

// Stamped on every member by the parser, so an unstamped node reads as public.
enum class Access : u8
{
    Public,
    Private
};

class Ast
{
public:
    // ---- Building ----

    Node_id add( Node_kind kind, Span span, u32 aux, std::span<const Node_id> children );

    // std::span has no braced-list constructor until C++26 (P2447), and the parser writes
    // add( ..., { lhs, rhs } ) constantly.
    Node_id add( Node_kind kind, Span span, u32 aux, std::initializer_list<Node_id> children )
    {
        return add( kind, span, aux, std::span<const Node_id>( children.begin(), children.size() ) );
    }

    void set_root( Node_id id );
    void set_access( Node_id id, Access access );
    void set_name_span( Node_id decl, Span span );
    void set_type_name_span( Node_id literal, Span span );

    // ---- Raw fields ----

    Node_id                  root() const;
    std::size_t              node_count() const;
    Node_kind                kind( Node_id id ) const;
    Span                     span( Node_id id ) const;
    u32                      aux( Node_id id ) const;
    std::span<const Node_id> children( Node_id id ) const; // invalidated by the next add()

    // One child, bounds-checked, where `children( id )[n]` is not.
    Node_id child( Node_id id, std::size_t index ) const;

    // ---- Side tables ----

    // Public for any node the parser did not stamp, so this never asks what kind `id` is.
    Access access( Node_id id ) const;

    // The span of a declaration's name, else of the whole declaration.
    Span name_span( Node_id decl ) const;

    // A struct literal's type name, which its own span runs past to the `}`.
    Span type_name_span( Node_id literal ) const;

    // ---- Parse failures ----

    // A failure at `at` breaks every node built afterwards whose span covers it.
    void fail( Span at );
    // An Error node, a node covering a failure, or a node with a broken child.
    bool broken( Node_id id ) const;
    bool failed_within( Span span ) const;

    // ---- aux, typed ----
    // Each asserts on a kind whose aux means something else.

    Symbol_id  name( Node_id id ) const;         // the named kinds; invalid where node.h allows
    Token_kind op( Node_id id ) const;           // Binary_expr, Unary_expr, Assign_stmt, Increment_stmt
    Keyword    keyword( Node_id id ) const;      // Mode_type, Marker_expr, Cast_expr
    Literal_id literal( Node_id id ) const;      // Int_, Float_, String_ and Char_literal
    bool       is_true( Node_id literal ) const; // Bool_literal
    bool       is_unsafe( Node_id block ) const;
    bool       is_default_arm( Node_id arm ) const;

    // ---- Children, by name ----
    // Each asserts on a kind without that slot, and returns an invalid id where node.h says the slot
    // may be absent.

    // Declarations.
    std::span<const Node_id> declarations( Node_id source_file ) const;
    Node_id                  package( Node_id id ) const; // Import_decl, Named_type, Struct_literal

    // Function-like declarations, and Function_type for the first three.
    Node_id                  return_type( Node_id id ) const; // invalid for a constructor or destructor
    Node_id                  param_list( Node_id id ) const;
    std::span<const Node_id> params( Node_id id ) const;          // the Param_decls, receiver included
    std::span<const Node_id> explicit_params( Node_id fn ) const; // past the receiver, when there is one
    Node_id                  body( Node_id id ) const;            // also While_stmt, For_stmt, Case_arm; invalid for `extern`

    // Type parameters and arguments.
    Node_id                  type_param_list( Node_id id ) const;    // aggregate, Enum_decl, function-like
    std::span<const Node_id> type_param_decls( Node_id list ) const; // empty for an invalid list; may hold Errors
    std::span<const Node_id> where_clauses( Node_id list ) const;
    std::span<const Node_id> bounds( Node_id where_clause ) const;
    Node_id                  type_arg_list( Node_id id ) const; // Generic_type, Call_expr, Name_expr, Path_expr
    std::span<const Node_id> type_args( Node_id id ) const;     // empty when no list was written

    // Aggregates and enums.
    std::span<const Node_id> members( Node_id id ) const;
    std::span<const Node_id> variants( Node_id id ) const;
    Node_id                  underlying_type( Node_id enum_decl ) const;
    std::span<const Node_id> payload( Node_id variant ) const;

    // Bindings: Param_decl, Var_decl, Field_decl.
    Node_id annotation( Node_id decl ) const;    // invalid for `auto`
    Node_id initialiser( Node_id decl ) const;   // Var_decl only
    Node_id declared_type( Node_id decl ) const; // the annotation, or what a function or Field_type returns

    // Types.
    Node_id inner_type( Node_id id ) const; // Pointer_, Many_pointer_, Mode_ and Const_type
    Node_id generic_name( Node_id generic_type ) const;

    // Expressions.
    Node_id                  lhs( Node_id binary ) const;
    Node_id                  rhs( Node_id binary ) const;
    Node_id                  operand( Node_id id ) const; // Unary_expr, Marker_expr, Cast_expr, Increment_stmt
    Node_id                  callee( Node_id call ) const;
    Node_id                  arg_list( Node_id call ) const;
    std::span<const Node_id> arguments( Node_id call ) const;
    Node_id                  object( Node_id id ) const; // Field_expr, Index_expr
    Node_id                  index( Node_id index_expr ) const;
    Node_id                  qualifier( Node_id path ) const;
    std::span<const Node_id> initialisers( Node_id id ) const;
    Node_id                  value( Node_id id ) const; // Return_stmt, Assign_stmt, Field_init
    Node_id                  target( Node_id assign ) const;
    Node_id                  written_type( Node_id id ) const; // Cast_expr, Alloc_expr
    Node_id                  pointer( Node_id id ) const;      // Free_expr, Destroy_expr
    Node_id                  count( Node_id id ) const;        // Alloc_expr, Destroy_expr; invalid when absent
    Node_id                  lower( Node_id range ) const;
    Node_id                  upper( Node_id range ) const;

    // Statements, and Conditional_expr for the first three.
    Node_id condition( Node_id id ) const; // also While_stmt, For_stmt, Assert_expr; invalid in an empty `for`
    Node_id then_branch( Node_id id ) const;
    Node_id else_branch( Node_id id ) const; // invalid when absent
    Node_id expression( Node_id expr_stmt ) const;
    Node_id init( Node_id for_stmt ) const;
    Node_id update( Node_id for_stmt ) const;

    // `switch`.
    Node_id                  scrutinee( Node_id switch_stmt ) const;
    std::span<const Node_id> arms( Node_id switch_stmt ) const;
    std::span<const Node_id> labels( Node_id arm ) const;
    Node_id                  variant_path( Node_id pattern ) const;
    std::span<const Node_id> bindings( Node_id pattern ) const;

private:
    std::vector<Node>    nodes_;
    std::vector<Node_id> children_; // every child of every node, back to back
    Node_id              root_;

    std::unordered_map<u32, Access> access_; // members only; absence means public
    std::unordered_map<u32, Span>   name_spans_;
    std::unordered_map<u32, Span>   type_name_spans_; // struct literals only

    std::vector<bool> broken_;
    std::vector<Span> failures_;
};

// The aggregate declaring `member`, or an invalid id for a top-level declaration.
Node_id enclosing_aggregate( const Ast& ast, Node_id member );

} // namespace keel
