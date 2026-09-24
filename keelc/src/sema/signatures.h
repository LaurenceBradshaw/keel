#pragma once
#include <string>
#include <string_view>
#include "ast/ast.h"
#include "common/interner.h"
#include "sema/aggregates.h"
#include "sema/annotations.h"
#include "sema/bounds.h"
#include "sema/generic_recursion.h"
#include "sema/overloads.h"
#include "sema/places.h"
#include "sema/reporter.h"
#include "sema/type.h"
#include "sema/types_builder.h"

// The declaration pass: what every top-level declaration contributes before any body is read, and
// the rules over an aggregate's members. Every signature is typed first so that a call can read its
// callee's parameter and return types straight out of the recorded types - the same reason the
// resolver makes two passes over file scope, mutual recursion (D18).
//
// It is a driver rather than a second checker: it sequences the pass and calls down into the same
// services a body walk uses.

namespace keel::sema
{

// A constructor and a destructor obey the same three rules and differ only in spelling, so the
// rules are written once over a description of the kind rather than twice over the kinds.
struct Member_kind
{
    Node_kind        node;
    std::string_view noun;     // "constructor"
    std::string_view prefix;   // written before the name: "" or "~"
    std::string_view remedy;   // what to do instead, when a struct declares one
    bool             only_one; // a second is refused here rather than by the overload rules
};

class Signatures
{
public:
    Signatures(
        const Ast&         ast,
        const Interner&    interner,
        Types_builder&     types,
        Aggregates&        aggregates,
        Bounds&            bounds,
        Annotations&       annotations,
        Places&            places,
        Generic_recursion& generic_recursion,
        Overloads&         overloads,
        Reporter&          reporter
    )
        : ast_( ast ),
          interner_( interner ),
          types_( types ),
          table_( types.table() ),
          aggregates_( aggregates ),
          bounds_( bounds ),
          annotations_( annotations ),
          places_( places ),
          generic_recursion_( generic_recursion ),
          overloads_( overloads ),
          reporter_( reporter )
    {
    }

    void declare();

private:
    // Declared in the order declare() runs them; each definition says what it depends on.
    void declare_structs();
    void declare_fields();
    void declare_functions();
    void declare_member_functions();
    void declare_globals();
    void declare_enums();

    // Ordering is Aggregates'; this only keeps D42 quiet about the aggregates it reported.
    void order_structs();

    // D29's rules, split out because check_struct_ownership cannot run before compute_owning has.
    void check_aggregate_members();
    void check_member_kind( Node_id decl, const Member_kind& kind );
    void check_aggregate_has_fields( Node_id decl );
    void check_enum_payloads();
    void check_struct_ownership();
    void check_struct_fields_are_not_owning( Node_id decl );

    const Ast&      ast_;
    const Interner& interner_;

    Types_builder& types_;
    Type_table&    table_;

    Aggregates&        aggregates_;
    Bounds&            bounds_;
    Annotations&       annotations_;
    Places&            places_;
    Generic_recursion& generic_recursion_;
    Overloads&         overloads_;
    Reporter&          reporter_;
};

} // namespace keel::sema
