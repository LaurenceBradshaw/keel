#pragma once
#include <unordered_set>
#include <vector>
#include "ast/ast.h"
#include "ast/node.h"
#include "common/interner.h"
#include "sema/reporter.h"
#include "sema/types_builder.h"

// Structs, classes and enums as *shapes*: what contains what, what owns a resource, and where a
// member or a field is. No rule about them lives here - only the answers the rules ask for.

namespace keel::sema
{

// Owns the containment order and the owning set, the two facts about an aggregate that are
// computed once for the whole program rather than at a use site. The owning set is what D29's
// rules about a `struct` are then read against; the containment order has no D-entry behind it.
class Aggregates
{
public:
    Aggregates( const Ast& ast, const Interner& interner, Types_builder& types, Reporter& reporter )
        : ast_( ast ),
          interner_( interner ),
          types_( types ),
          table_( types.table() ),
          reporter_( reporter )
    {
    }

    // The DFS post-order over by-value containment, reporting any cycle it finds. Returns the
    // aggregates that were on one, so the caller can keep its own rules quiet about them.
    std::vector<Node_id> order_structs();

    // Must run after order_structs, whose order it walks.
    void compute_owning();

    // Whether this concrete aggregate owns a resource. False for anything else, a type parameter
    // included - ask Bounds when the answer has to cover one.
    bool owns( Type_id type ) const;

    // The first member of a kind, or invalid. Constructors and destructors are both at most one,
    // so "the first" and "the only" coincide once check_aggregate_members has run.
    Node_id find_member( Node_id decl, Node_kind kind ) const;
    Node_id find_method( Node_id decl, Symbol_id name ) const;
    Node_id find_field( Type_id type, Symbol_id name ) const;

    // What binds an aggregate's type parameters to what it was instantiated at. Empty for a
    // non-generic one, and the identity for the open form, so callers need no branch.
    Bindings bindings_of( Type_id aggregate ) const;

    // A field's type *as seen through this instance*. The declared type of `Box<T>`'s field is `T`,
    // which is true of the template and of no value anyone holds - so every read of a field's type
    // goes through here rather than through the recorded type of the field declaration.
    Type_id field_type( Type_id aggregate, Node_id field );

    // Handing the run's result to Types. Both leave this object empty; nothing reads it after.
    std::vector<Node_id>    take_struct_order();
    std::unordered_set<u32> take_owning();

private:
    void report_containment_cycle( Node_id decl, const std::vector<Node_id>& path );
    bool contains_itself( Node_id decl, std::vector<Node_id>& path );

    const Ast&      ast_;
    const Interner& interner_;
    Types_builder&  types_;
    Type_table&     table_;
    Reporter&       reporter_;

    std::vector<Node_id>    struct_order_; // dependencies first; also the "already proved acyclic" set
    std::unordered_set<u32> owning_;       // filled by compute_owning, handed to Types
};

} // namespace keel::sema
