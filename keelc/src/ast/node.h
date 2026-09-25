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
    // arity stays fixed; later passes skip Error subtrees silently.
    Error,

    Source_file,
    Function_decl,
    Destructor_decl,
    Constructor_decl,
    Param_decl,
    Named_type,
    Pointer_type,
    Mode_type,
    Const_type,
    Generic_type,
    Type_arg_list,
    Param_list,
    Block,
    Return_stmt,
    Int_literal,
    Float_literal,
    String_literal,
    Char_literal,
    Bool_literal,
    Name_expr,
    Binary_expr,
    Unary_expr,
    Conditional_expr,
    Call_expr,
    Arg_list,
    Var_decl,
    Assign_stmt,
    Increment_stmt,
    Expr_stmt,
    If_stmt,
    While_stmt,
    For_stmt,
    Struct_decl,
    Class_decl,
    Enum_decl,    // aux is the name; children are Variant_decls, then the underlying type if written
    Variant_decl, // aux is the name; no children until payloads (D7)
    Path_expr,    // `Colour::Red`; aux is the name, child 0 the qualifier, child 1 its type arguments
    Field_decl,
    Field_expr,
    Struct_literal,
    Field_init,
    Marker_expr, // `move`, `out`, `ref` - a unary operator that does not change the type of its operand
    Null_literal,
    Cast_expr,
    Break_stmt,
    Continue_stmt,
    Switch_stmt, // children: scrutinee, then Case_arms in source order
    Case_arm,    // children: the labels (Path_exprs), then the body Block; aux is 1 for `default`
    Range_expr,
    Variant_pattern, // `Shape::Circle( r )` in a case; children are the Path_expr then Binding_decls
    Binding_decl,    // a name bound by a pattern; aux is the name, and it has no annotation  // `1..5`, half-open; children are
                     // the two bounds
    Method_decl,     // aux is the name; children are { return type, params, body }, as Function_decl
    Alloc_expr,      // `alloc<T>()`; child 0 is the type annotation, and there is no operand
    Free_expr,       // `free( p )`; child 0 is the pointer
    Fallthrough_stmt,
    // The whole generic declaration: the parameters, then the `where` clauses constraining them.
    // One node rather than two so that a function-like declaration keeps four children, and so the
    // resolver's single visit declares the parameters before anything names one.
    Type_param_list,
    Type_param_decl, // aux is the name; the bounds are in a Where_clause beside it, not below it
    Where_clause,    // aux is the parameter it constrains; children are Bound_names
    Bound_name,      // aux is the bound's name, resolved against D40's fixed set by the checker

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
