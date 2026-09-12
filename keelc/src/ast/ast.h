#pragma once
#include <initializer_list>
#include <span>
#include <vector>
#include "ast/node.h"

namespace keel
{

class Ast
{
public:
    Node_id add( Node_kind kind, Span span, u32 aux, std::span<const Node_id> children );

    // std::span cannot be built from a braced list until C++26 (P2447), and the parser writes
    // add( ..., { lhs, rhs } ) constantly for the fixed-arity kinds.
    Node_id add( Node_kind kind, Span span, u32 aux, std::initializer_list<Node_id> children )
    {
        return add( kind, span, aux, std::span<const Node_id>( children.begin(), children.size() ) );
    }

    Node_kind                kind( Node_id id ) const;
    Span                     span( Node_id id ) const;
    u32                      aux( Node_id id ) const;
    std::span<const Node_id> children( Node_id id ) const;

    Node_id     root() const;
    void        set_root( Node_id id );
    std::size_t node_count() const;

private:
    std::vector<Node>    nodes_;
    std::vector<Node_id> children_; // every child of every node, back to back
    Node_id              root_;
};

} // namespace keel