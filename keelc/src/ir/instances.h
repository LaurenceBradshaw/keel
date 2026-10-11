// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <vector>
#include "ast/ast.h"
#include "sema/type.h"
#include "sema/type_checker.h"

namespace keel
{

// Monomorphisation: every closed instance of a generic the program reaches, each once. Types is
// mutable because substituting an edge's arguments can intern a type that does not exist yet.
//
// The checker's list is only the seed. A call site inside a generic writes `g<T>`, which names no
// instance at all until `T` is known - so the real set is the closure of that list under the
// generic call graph, and reaching it needs a worklist rather than a pass.
//
// It terminates because Generic_recursion::check refused the shape that would not: a cycle whose
// arguments grow a level each time round. Every other cycle forwards its parameters unchanged, so
// going round it twice produces an instantiation that is already in the set.
class Instance_queue
{
public:
    Instance_queue( const Ast& ast, Types& types );

    // Called between lowerings: lowering one instance can intern the type whose destructor or
    // conformance is the next.
    std::optional<Instantiation> next();

private:
    void queue_new_composites();

    const Ast&                 ast_;
    Types&                     types_;
    std::vector<Instantiation> pending_ {};
    std::size_t                at_              = 0;
    std::size_t                composites_seen_ = 0;
};

// The instance's type arguments bound to its declaration's parameters, positionally.
Bindings bindings_for( const Ast& ast, const Types& types, const Instantiation& instance );

} // namespace keel
