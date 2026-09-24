#pragma once
#include "ast/ast.h"
#include "common/interner.h"
#include "sema/annotations.h"
#include "sema/bounds.h"
#include "sema/reporter.h"
#include "sema/resolver.h"
#include "sema/type.h"
#include "sema/type_checker.h"
#include "sema/types_builder.h"

namespace keel
{
namespace sema
{

// What an argument says about itself before a candidate has been chosen.
enum class Argument_kind : u8
{
    Typed,
    Integer,
    Floating,
    Anything
};

// A kind other than `Typed` carries no type, deliberately: that is where D26's rule that a literal
// says nothing actually lives, because deduction and selection both read the type and find none.
struct Argument_shape
{
    Argument_kind kind     = Argument_kind::Anything;
    Type_id       type     = {};
    Keyword       marker   = Keyword::Count;
    bool          recorded = false; // whether infer() has already run over it
};

// One argument the caller still has to walk. Returned rather than walked here, which is the whole
// of the no-re-entry rule for this class: an invalid `expected` is a real expectation - a
// parameter whose own type failed to resolve holds one - so the flag says which call to make.
struct Argument_work
{
    Node_id argument;
    Type_id expected;
    bool    with_expectation = false;
};

// Which callable a call names, and what its arguments then have to be. None of these re-enters the
// expression walk, which is the rule every class below it obeys: selection takes the argument
// *shapes* the caller reduced them to, and argument checking hands the walk back a list to perform.
// Declaration subtrees it does read: a parameter list is not an expression and cannot call back.
class Overloads
{
public:
    Overloads(
        const Ast&        ast,
        const Interner&   interner,
        const Resolution& resolution,
        Types_builder&    types,
        const Aggregates& aggregates,
        const Bounds&     bounds,
        Annotations&      annotations,
        Reporter&         reporter
    )
        : ast_( ast ),
          interner_( interner ),
          resolution_( resolution ),
          types_( types ),
          table_( types.table() ),
          aggregates_( aggregates ),
          bounds_( bounds ),
          annotations_( annotations ),
          reporter_( reporter )
    {
    }

    Bindings type_bindings( Node_id callable, std::span<const Type_id> arguments ) const;

    bool deduce_type_arguments(
        Node_id                         callable,
        u32                             implicit_params,
        std::span<const Argument_shape> shapes,
        std::span<const Node_id>        arguments,
        Type_id                         result,
        Type_id                         expectation,
        std::string_view                name,
        Span                            at,
        std::vector<Type_id>&           resolved,
        std::vector<Node_id>&           bound_by
    );

    // Selection is two calls because it is lazy, and the laziness is a rule rather than an
    // optimisation: what the call *wrote* narrows the set before anything is typed, and typing an
    // argument is what cannot be taken back. Empty means it is settled and reported; one means
    // settled; more than one means the caller reduces the arguments to shapes and asks below.
    //
    // `instance` is the aggregate a construction is for, so its candidate list can spell `Box<i32>`'s
    // parameters rather than the declaration's `T`. Invalid for a free function, which is named
    // rather than instantiated - the method pair below takes the receiver for the same reason.
    std::vector<Node_id> viable_overloads(
        Node_id call, std::string_view name, std::span<const Node_id> candidates, u32 implicit_params, Type_id instance
    );
    std::vector<Node_id> viable_methods( Node_id call, Node_id first, Type_id receiver );

    Node_id select_overload(
        Node_id                         call,
        std::string_view                name,
        std::span<const Node_id>        viable,
        u32                             implicit_params,
        Type_id                         instance,
        std::span<const Argument_shape> shapes,
        std::vector<Type_id>&           resolved
    );
    Node_id select_method(
        Node_id call, Node_id first, Type_id receiver, std::span<const Node_id> viable, std::span<const Argument_shape> shapes
    );

    // Everything about the arguments that is the callable's business - arity, types, and D2's
    // markers - reported here; the walking the answers imply is handed back in order.
    std::vector<Argument_work> check_call_arguments(
        Node_id                         call,
        Node_id                         callable,
        std::string_view                name,
        u32                             implicit_params,
        const Bindings&                 bindings,
        std::span<const Argument_shape> shapes
    );
    void check_argument_markers(
        Node_id call, Node_id callable, std::string_view name, u32 implicit_params, const Bindings& bindings
    );

    void check_overload_sets();

    // Which instantiation each generic call resolved to. Carried rather than recomputed, for the
    // reason callees_ is: resolving a type argument to a type is this pass's rule, and lowering
    // repeating it is how the two would drift. Shaped after Types_builder's record and take.
    void                         record_instantiation( Node_id call, std::size_t instance );
    std::unordered_map<u32, u32> take_instantiations();

private:
    bool marker_accepts( Node_id param, Keyword given, Type_id expected );
    bool candidate_accepts(
        Node_id callable, u32 implicit_params, std::span<const Argument_shape> shapes, const Bindings& bindings
    );

    bool deduce_for_candidate(
        Node_id callable, u32 implicit_params, std::span<const Argument_shape> shapes, std::vector<Type_id>& resolved
    );

    // The signature as a call site would have to write it, and a list of them - both read four
    // collaborators, which is what keeps them members rather than file-local free functions.
    std::string signature_of( Node_id callable, u32 implicit_params, const Bindings& bindings );
    std::string candidate_list( std::span<const Node_id> candidates, u32 implicit_params, const Bindings& bindings );

    bool parameters_collide( Node_id first, Node_id second, bool member );
    void check_overloaded_pair( Node_id first, Node_id second, bool member );

    void check_aggregate_overloads( Node_id decl );

    const Ast&           ast_;
    const Interner&      interner_;
    const Resolution&    resolution_;
    const Types_builder& types_;
    Type_table&          table_;
    const Aggregates&    aggregates_;
    const Bounds&        bounds_;
    Annotations&         annotations_;
    Reporter&            reporter_;

    std::unordered_map<u32, u32> instantiation_of_;
};

} // namespace sema
} // namespace keel
