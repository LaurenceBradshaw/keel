// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <cstddef>
#include <vector>
#include "ast/ast.h"
#include "ir/builder.h"
#include "sema/type_checker.h"

namespace keel
{

// D2's drop of one place, expanded: a destructor of its own, then each field in reverse. Free
// because an enum's synthesised destructor drops its payload the same way and has no Lowering.
void drop_place( Builder& builder, const Ast& ast, Types& types, Place place, Type_id type, Span span, bool replacing );

// Which locals are live and when each is dropped: a statement's owning temporaries at its end, a
// scope's locals when it closes or a jump leaves it. Nothing else touches the two lists.
class Drop_scopes
{
public:
    Drop_scopes( Builder& builder, const Ast& ast, Types& types );

    // A temporary the statement built, dropped at its end. A no-op unless its type owns.
    void        own_temporary( Local_id local );
    std::size_t mark() const;
    // Each temporary listed since `mark` was built in a branch, so its drop needs a flag.
    void one_path_since( std::size_t mark );
    // A temporary whose lifetime is now the innermost scope's; a no-op if it is not one.
    void adopt_into_scope( Local_id local );

    // Asserts the last statement dropped everything it built.
    void begin_statement() const;
    // Drop in reverse, end storage, clear: ending storage resets a one-path flag for a loop's next turn.
    void end_statement( Span span );
    // The same for the temporaries listed since `mark`: a fill's value, built once per turn.
    void end_since( std::size_t mark, Span span );
    // Drop but keep: a `try`'s error exit, since the `ok` path still owns them.
    void drop_built( Span span );

    // A local the innermost scope drops, if it owns, and ends the storage of.
    void add_to_scope( Local_id local );
    u32  depth() const;
    void push_scope();
    void pop_scope( Span span );
    // Drop and end every scope local above depth's mark, without popping: `break`, `continue`, `return`.
    void unwind_to( u32 depth, Span span );

private:
    void drop( Local_id local, Span span );

    Builder&   builder_;
    const Ast& ast_;
    Types&     types_;

    std::vector<Local_id> temporaries_;
    std::vector<Local_id> scope_locals_;
    std::vector<u32>      marks_;
};

} // namespace keel
