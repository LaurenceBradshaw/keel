#pragma once
#include "ast/ast.h"
#include "common/interner.h"
#include "lex/token.h"
#include "sema/bounds.h"
#include "sema/callees.h"
#include "sema/reporter.h"
#include "sema/resolver.h"
#include "sema/types_builder.h"

namespace keel
{

namespace sema
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
    bool check_writable( Node_id target, Node_id current_function );
    bool is_borrow_binding( Node_id decl ) const;

    // Parameter 0 of a method, constructor or destructor - the synthesised `this`. Invalid for a
    // free function, which has no receiver for a bare name to be rooted in.
    Node_id receiver_of( Node_id function ) const;

    Node_id place_root( Node_id id, Node_id current_function ) const;

    // D31's initialisation and assignment clause: an owning value transfers rather than copies, and
    // the transfer is written down.
    void check_owning_source( Node_id value, Type_id type );

    // D31: which parameters travel by address. Its own pass because the owning query is only
    // answered after compute_owning, by which time both parameter loops have already run.
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

} // namespace sema

} // namespace keel
