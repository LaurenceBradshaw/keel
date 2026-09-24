#pragma once
#include <string>
#include "ast/ast.h"
#include "common/interner.h"
#include "sema/annotations.h"
#include "sema/bounds.h"
#include "sema/constant_folder.h"
#include "sema/coverage.h"
#include "sema/expressions.h"
#include "sema/places.h"
#include "sema/reporter.h"
#include "sema/resolver.h"
#include "sema/type.h"
#include "sema/types_builder.h"

namespace keel
{
namespace sema
{

// The statement and declaration walk: one member per construct, and the dispatch that reaches them.
// Every rule it applies belongs to a class above it - this is the shape of the tree, not the rules.
//
// It is the only class that may visit, which is why the three switch members and the arm loop that
// drives Coverage live here, and why `break`, `continue` and `fallthrough` are reported from the
// dispatch rather than from any of them: their depths are scoped to a body this class walks.
class Statements
{
public:
    Statements(
        const Ast&        ast,
        const Interner&   interner,
        const Resolution& resolution,
        Types_builder&    types,
        const Bounds&     bounds,
        Annotations&      annotations,
        Constant_folder&  constant_folder,
        Places&           places,
        Expressions&      expressions,
        Coverage&         coverage,
        Reporter&         reporter
    )
        : ast_( ast ),
          interner_( interner ),
          resolution_( resolution ),
          types_( types ),
          table_( types.table() ),
          bounds_( bounds ),
          annotations_( annotations ),
          constant_folder_( constant_folder ),
          places_( places ),
          expressions_( expressions ),
          coverage_( coverage ),
          reporter_( reporter )
    {
    }

    // The entry point for the statement and declaration walk. The expression half is
    // `Expressions::infer` and `Expressions::check`, which nothing here may reach into.
    void visit( Node_id id );

private:
    // One per construct, dispatched from visit - the shape parse_*() uses next door.
    void visit_function( Node_id id );
    void visit_return( Node_id id );
    void visit_var( Node_id id );
    void visit_assign( Node_id id );
    void visit_if( Node_id id );
    void visit_switch( Node_id id );
    void visit_while( Node_id id );
    void visit_for( Node_id id );
    void visit_increment( Node_id id );
    void visit_global( Node_id id );
    void visit_block( Node_id id );

    const Ast&        ast_;
    const Interner&   interner_;
    const Resolution& resolution_;
    Types_builder&    types_;
    const Type_table& table_;
    const Bounds&     bounds_;
    Annotations&      annotations_;
    Constant_folder&  constant_folder_;
    Places&           places_;
    Expressions&      expressions_;
    Coverage&         coverage_;
    Reporter&         reporter_;

    Type_id current_return_;

    u32 loop_depth_      = 0; // `continue` binds here, looking past any switch
    u32 breakable_depth_ = 0; // `break` binds to the nearest of either
};

} // namespace sema
} // namespace keel
