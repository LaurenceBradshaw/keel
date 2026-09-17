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

    // One child, bounds-checked. `children( id )[2]` is the idiom everywhere and its `operator[]`
    // is unchecked, so reading a slot a kind does not have is silent corruption rather than a
    // failure - twice over while type parameters were being added. Prefer this wherever the index
    // is a constant naming a fixed slot.
    Node_id child( Node_id id, std::size_t index ) const;

    // The members of a `struct` or `class` - its fields, methods, constructor and destructor - and
    // nothing else. A generic aggregate carries its type parameters in the same child list, in a
    // leading slot this skips, exactly as an `enum` carries its underlying type in one; walking the
    // children directly would read that slot as a member. Asserts on any other kind, because the
    // answer for one would be a guess about what its children mean.
    std::span<const Node_id> members( Node_id id ) const;

    // The declaration's `Type_param_list`, or an invalid id when it has none. Which child holds it
    // depends on the kind - the front for an aggregate, whose members are variadic, and the fixed
    // last slot for anything function-like - and this is the one place that knows, so no caller
    // indexes that slot by hand.
    Node_id type_param_list( Node_id id ) const;

    Node_id     root() const;
    void        set_root( Node_id id );
    std::size_t node_count() const;

private:
    std::vector<Node>    nodes_;
    std::vector<Node_id> children_; // every child of every node, back to back
    Node_id              root_;
};

} // namespace keel