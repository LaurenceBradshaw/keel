#pragma once
#include <optional>
#include <unordered_set>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "sema/aggregates.h"
#include "sema/constant_folder.h"
#include "sema/expressions.h"
#include "sema/reporter.h"
#include "sema/type.h"
#include "sema/types_builder.h"

namespace keel
{
namespace sema
{

// PLAN D34. A `case` label over a number is an interval, half-open: `case 3:` is [3, 4) and
// `case 1..5:` is [1, 5). One representation for both is what makes overlap a single pairwise walk
// rather than three cases - value/value, value/range and range/range.
struct Interval
{
    i64     low  = 0;
    i64     high = 0; // exclusive
    Node_id label {};
};

// What one `switch` accumulates while its arms are checked. The caller holds it because the arm
// bodies are visited between arms and only the caller may visit - see the class comment.
struct Switch_coverage
{
    Node_id               switch_id {};
    Type_id               type {};
    bool                  numeric     = false;
    bool                  floating    = false;
    bool                  has_default = false;
    std::vector<Node_id>  variants;  // the label that covered each ordinal, empty over a number
    std::vector<Interval> intervals; // what the labels cover, empty over an enum
};

// Whether a `switch`'s labels account for its scrutinee, and whether its arms are shaped the way a
// language without run-on `case`s needs them to be. Everything about a label; nothing about a body.
//
// Bodies are the inversion this class exists to force. The check has to interleave with them - an
// arm's pattern declares names the arm's body then reads - so the loop over arms lives in
// `Statements`, which may visit, and this class answers one arm at a time - nothing here re-enters
// the walk.
class Coverage
{
public:
    Coverage(
        const Ast&             ast,
        const Interner&        interner,
        Types_builder&         types,
        Aggregates&            aggregates,
        const Constant_folder& constant_folder,
        Expressions&           expressions,
        Reporter&              reporter
    )
        : ast_( ast ),
          interner_( interner ),
          types_( types ),
          table_( types.table() ),
          aggregates_( aggregates ),
          constant_folder_( constant_folder ),
          expressions_( expressions ),
          reporter_( reporter )
    {
    }

    // The rules about an arm's shape rather than its labels' values, so they hold for an enum
    // switch and a numeric one alike. Runs before any arm body is visited, which is what lets
    // ruled_fallthrough below be an answer rather than a guess.
    void check_arm_structure( Node_id id );

    // The three halves of one loop the caller drives: what the scrutinee settles before any arm,
    // one arm's labels, and what only the whole arm list can say.
    Switch_coverage begin_switch( Node_id id, Type_id type );
    void            check_arm_labels( Node_id arm, Switch_coverage& state );
    void            finish_switch( const Switch_coverage& state );

    // Whether check_arm_structure has ruled on this `fallthrough` - which is exactly those written
    // as a top-level statement of an arm. Any other one is nested inside a block, a loop, or no
    // `switch` at all, and the statement walk reports it; without this the lowerer would reach it
    // with no target.
    bool ruled_fallthrough( Node_id id ) const
    {
        return ruled_fallthroughs_.contains( id.v );
    }

private:
    void check_enum_arm_labels( Node_id arm, Switch_coverage& state );
    void check_numeric_arm_labels( Node_id arm, Switch_coverage& state );
    void finish_enum_switch( const Switch_coverage& state );
    void finish_numeric_switch( const Switch_coverage& state );

    void check_variant_pattern( Node_id pattern, Type_id type, std::vector<Node_id>& covered );

    std::optional<i64> fold_bound( Node_id bound, Type_id expected );

    // Whether control can reach the end of this statement. Conservative one way only: it answers
    // "yes" when unsure, so the rule below can miss a divergence but never invent one.
    bool completes_normally( Node_id id ) const;

    // Whether any of an arm's labels destructures a payload. What "this arm binds" means for the
    // two rules that ask it: labels may not be stacked when one does, and nothing may fall into one.
    bool arm_binds( Node_id arm ) const;

    const Ast&             ast_;
    const Interner&        interner_;
    Types_builder&         types_;
    const Type_table&      table_;
    Aggregates&            aggregates_;
    const Constant_folder& constant_folder_;
    Expressions&           expressions_;
    Reporter&              reporter_;

    std::unordered_set<u32> ruled_fallthroughs_;
};

} // namespace sema
} // namespace keel
