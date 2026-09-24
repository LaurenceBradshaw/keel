#pragma once
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "sema/reporter.h"
#include "sema/type_checker.h"

namespace keel
{
namespace sema
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
    void        record_generic_uses( Node_id from, Type_id type, Span at );
    std::size_t record_instantiation( Node_id declaration, std::vector<Type_id> arguments );
    void        mark_reported( Node_id declaration );

    void check();

    std::vector<Instantiation> take_instantiations();
    std::vector<Generic_call>  take_generic_calls();

private:
    bool expands_forever( Node_id generic, std::vector<Node_id>& path, std::optional<Span> grown_at );
    bool wraps_a_parameter( Type_id argument ) const;

    const Ast&        ast_;
    const Interner&   interner_;
    const Type_table& table_;
    Reporter&         reporter_;

    // Every declaration on a cycle already named. One cycle is one mistake, and the walk below
    // starts from each of its own members in turn - so without this it is reported once per member.
    std::unordered_set<u32> expanding_;

    // Every (generic, type arguments) pair a call site named, deduplicated. Nothing reads it yet:
    // it is what the monomorphisation worklist starts from, recorded here because this is the one
    // pass that turns a type argument into a type.
    std::vector<Instantiation> instantiations_;

    // The generic call graph, in declaration order. See Generic_call.
    std::vector<Generic_call> generic_calls_;
};

} // namespace sema
} // namespace keel
