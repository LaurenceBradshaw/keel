// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <cstddef>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "sema/reporter.h"
#include "sema/type_checker.h"

namespace keel::sema
{

// D11's other termination question. Monomorphisation emits one instance per set of type arguments,
// so it finishes only if that set is finite; this owns the graph that decides it.
class Generic_recursion
{
public:
    Generic_recursion( const Ast& ast, const Interner& interner, const Type_table& table, Reporter& reporter )
        : ast_( ast ),
          interner_( interner ),
          table_( table ),
          reporter_( reporter )
    {
    }

    void        record_call( Node_id from, Node_id to, std::vector<Type_id> arguments, Span at );
    void        absorb_call( Generic_call call ); // a loaded module's, unless the declaration pass recorded it again
    void        record_generic_uses( Node_id from, Type_id type, Span at );
    void        record_bodies( Node_id id, Node_id from, std::span<const Type_id> types ); // after the bodies are typed
    std::size_t record_instantiation( Node_id declaration, std::vector<Type_id> arguments );
    void        mark_reported( Node_id declaration );

    void check();

    std::vector<Instantiation> take_instantiations();
    std::vector<Generic_call>  take_generic_calls();

private:
    // A generic on the current walk, and the growing edge that entered it.
    struct Step
    {
        Node_id             generic;
        std::optional<Span> grown_at;
    };

    void record_uses( Node_id from, Type_id type, Span at, std::vector<Generic_call>& into );
    bool expands_forever( Node_id generic, std::vector<Step>& path );
    bool wraps_a_parameter( Type_id argument ) const;

    const Ast&        ast_;
    const Interner&   interner_;
    const Type_table& table_;
    Reporter&         reporter_;

    // Every declaration on a cycle already named. One cycle is one mistake, and the walk below
    // starts from each of its own members in turn - so without this it is reported once per member.
    std::unordered_set<u32> expanding_;

    std::unordered_set<u32>   seen_;    // the types record_bodies already took from the current body
    std::vector<Generic_call> implied_; // edges for check() alone: types named in bodies, and destructors

    // Every (generic, type arguments) pair a call site named, deduplicated. Nothing reads it yet:
    // it is what the monomorphisation worklist starts from, recorded here because this is the one
    // pass that turns a type argument into a type.
    std::vector<Instantiation> instantiations_;

    // The generic call graph, in declaration order. See Generic_call.
    std::vector<Generic_call> generic_calls_;
};

} // namespace keel::sema
