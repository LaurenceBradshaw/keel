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
    Param_decl,
    Named_type,
    Param_list,
    Block,
    Return_stmt,
    Int_literal,
    Binary_expr,

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
