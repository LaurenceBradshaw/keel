// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ast/ast.h"
#include "common/interner.h"
#include "lex/token.h"
#include "sema/bounds.h"
#include "sema/callees.h"
#include "sema/reporter.h"
#include "sema/resolver.h"
#include "sema/types_builder.h"

namespace keel::sema
{

class Places
{
public:
    Places(
        const Ast&        ast,
        const Interner&   interner,
        const Resolution& resolution,
        Types_builder&    types,
        Bounds&           bounds,
        const Callees&    callees,
        Reporter&         reporter
    )
        : ast_( ast ),
          interner_( interner ),
          resolution_( resolution ),
          types_( types ),
          table_( types.table() ),
          bounds_( bounds ),
          callees_( callees ),
          reporter_( reporter )
    {
    }

    bool is_assignable( Node_id id ) const;
    bool returns_a_binding( Node_id decl ) const;
    bool call_returns_a_binding( Node_id call ) const;
    bool check_writable( Node_id target, Node_id current_function, bool replaces = false );
    bool is_read_only( Node_id target, Node_id current_function ) const;
    // The pointer a place is reached through when that pointer points to const, or invalid.
    Node_id through_const_pointer( Node_id place ) const;
    // The `const` field a place is, or is part of, short of any pointer; or invalid.
    Node_id const_field( Node_id place ) const;
    // Whether `place` is a constructor's own `const` field, written whole.
    bool initialises_const_field( Node_id place, Node_id current_function ) const;
    bool is_borrow_binding( Node_id decl ) const;

    // Parameter 0 of a method, constructor or destructor - the synthesised `this`. Invalid for a
    // free function, which has no receiver for a bare name to be rooted in.
    Node_id receiver_of( Node_id function ) const;

    Node_id place_root( Node_id id, Node_id current_function ) const;
    Node_id place_source( Node_id id ) const;
    bool    is_operator_index( Node_id id ) const;
    Node_id operator_projection( Node_id place ) const;

    // The declaration whose storage ends when `current_function` returns, or invalid.
    Node_id dying_storage( Node_id place, Node_id current_function ) const;

    // D31's initialisation and assignment clause: an owning value transfers rather than copies, and
    // the transfer is written down.
    void check_owning_source( Node_id value, Type_id type );
    bool check_owning_return( Node_id value, Type_id type );

    // D31: which parameters travel by address. Its own pass because ownership is asked of field
    // types, which are recorded only after both parameter loops have run.
    void record_borrowed_parameters();
    void record_binding_address( Node_id annotation, Type_id type );

private:
    const Ast&        ast_;
    const Interner&   interner_;
    const Resolution& resolution_;
    Types_builder&    types_;
    Type_table&       table_;
    Bounds&           bounds_;
    const Callees&    callees_;
    Reporter&         reporter_;
};

} // namespace keel::sema
