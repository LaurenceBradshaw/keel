#pragma once
#include <string_view>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "sema/bounds.h"
#include "sema/reporter.h"
#include "sema/resolver.h"
#include "sema/types_builder.h"

namespace keel
{
namespace sema
{

// What the author wrote in type position, read. The two members call each other - `Box<Vec<i32>>`
// is one recursion across both - so they are one class rather than two, which is the whole reason
// this one exists: it owns no state, and a mutual recursion is the one thing that still needs one.
class Annotations
{
public:
    Annotations(
        const Ast&        ast,
        const Interner&   interner,
        const Resolution& resolution,
        Types_builder&    types,
        Bounds&           bounds,
        Reporter&         reporter
    )
        : ast_( ast ),
          interner_( interner ),
          resolution_( resolution ),
          types_( types ),
          table_( types.table() ),
          bounds_( bounds ),
          reporter_( reporter )
    {
    }

    // Computes the type an annotation subtree spells, where Types_builder::type_of reads back one
    // already recorded. Invalid for `auto`, which the parser writes as an invalid Node_id.
    // `outermost` is false inside a wrapper, which is what tells a pointer to `const` from a
    // `const` binding.
    Type_id type_of( Node_id annotation, bool outermost = true );

    // False only when the *count* is wrong, which is the one failure that leaves nothing usable
    // behind. An argument that failed to resolve, or that broke a bound, is reported and still
    // handed back, so the rest of the annotation or call is checked against something.
    bool
    resolve_type_arguments( Node_id declaration, Node_id type_args, std::string_view name, std::vector<Type_id>& resolved );

private:
    const Ast&        ast_;
    const Interner&   interner_;
    const Resolution& resolution_;
    Types_builder&    types_;
    Type_table&       table_;
    Bounds&           bounds_;
    Reporter&         reporter_;
};

} // namespace sema
} // namespace keel
