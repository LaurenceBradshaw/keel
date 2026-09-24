#pragma once
#include <cstddef>
#include <span>
#include <vector>
#include "ast/node.h"
#include "sema/type.h"

// The interned types, and the type each node was given.

namespace keel::sema
{

// The two pieces of state every rule class reads. Owned here rather than by any of them, and
// reachable without naming the checker - which is what lets the rest of the checker be a stack of
// classes that only ever name the ones below them.
//
// `record` is the only way to write a node's type, so forgetting to write one is hard rather than silent.
class Types_builder
{
public:
    // Sized once, at the start of the run, like bindings_ in Resolver.
    void size_to( std::size_t node_count );

    Type_table& table()
    {
        return table_;
    }

    const Type_table& table() const
    {
        return table_;
    }

    // Writes the node's type and returns it, so an infer branch can end in one of these.
    Type_id record( Node_id id, Type_id type );

    // Invalid when the node was never typed - a statement, a type annotation, an error subtree, or
    // a node this pass has not reached yet.
    Type_id type_of( Node_id id ) const;

    // The whole vector, for the free functions that take one rather than a checker.
    std::span<const Type_id> recorded() const;

    // Handing the run's result to Types. Both leave this object empty; nothing reads it after.
    Type_table           take_table();
    std::vector<Type_id> take_types();

private:
    Type_table           table_;
    std::vector<Type_id> types_; // sized node_count(), invalid-filled
};

} // namespace keel::sema
