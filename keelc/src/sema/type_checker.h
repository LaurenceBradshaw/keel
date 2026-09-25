#pragma once
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ast/ast.h"
#include "ast/node.h"
#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "sema/resolver.h"
#include "sema/type.h"

namespace keel
{

// A file-scope initialiser, evaluated. Two kinds cover everything Keel allows there: a bool is the
// integer 0 or 1, and `nullptr` is the integer 0. Integers keep the sign-magnitude form fits()
// already takes, so the range checks and the folder agree without converting between them.
struct Constant_value
{
    enum class Kind : u8
    {
        Integer,
        Float
    };

    Kind kind      = Kind::Integer;
    u64  magnitude = 0;
    bool negative  = false;
    f64  floating  = 0.0;
};

// A generic named with concrete type arguments at some call site. The seed for M6's
// monomorphisation worklist: recorded by the checker because that is the only pass that resolves a
// type argument to a type, and read by nothing yet.
struct Instantiation
{
    Node_id              declaration {};
    std::vector<Type_id> arguments;
};

// One edge of the generic call graph: a generic body naming another generic. The arguments are
// written in the *caller's* type parameters, so `g<T>( a )` inside `f<T>` records `T` and not
// anything concrete - which is the whole reason the edge is worth keeping. Two passes read it:
// the checker, to refuse a cycle that would expand forever, and the lowerer, to find the
// instantiations no call site ever wrote down.
struct Generic_call
{
    Node_id              from {}; // the generic whose body holds the call
    Node_id              to {};   // the generic it names
    std::vector<Type_id> arguments;
    Span                 at {};
};

class Types
{
public:
    Types() = default;
    Types(
        Type_table                              table,
        std::vector<Type_id>                    types,
        std::vector<Node_id>                    struct_order,
        std::unordered_map<u32, Constant_value> constants,
        std::unordered_set<u32>                 owning,
        std::unordered_map<u32, Node_id>        callees,
        std::vector<Instantiation>              instantiations,
        std::unordered_map<u32, u32>            instantiation_of,
        std::vector<Generic_call>               generic_calls
    )
        : table_( std::move( table ) ),
          types_( std::move( types ) ),
          struct_order_( std::move( struct_order ) ),
          constants_( std::move( constants ) ),
          callees_( std::move( callees ) ),
          owning_( std::move( owning ) ),
          instantiations_( std::move( instantiations ) ),
          instantiation_of_( std::move( instantiation_of ) ),
          generic_calls_( std::move( generic_calls ) )
    {
    }

    // Invalid when the node was never typed - a statement, a type annotation, or an error subtree.
    Type_id type_of( Node_id node ) const
    {
        return node.v < types_.size() ? types_[node.v] : Type_id {};
    }

    // Every node's recorded type, for the two free functions that want the whole vector rather than
    // one entry - they are also called from inside the checker, which has no Types yet.
    std::span<const Type_id> recorded() const
    {
        return types_;
    }

    const Type_table& table() const
    {
        return table_;
    }

    // Mutable because substituting a type parameter can intern a type that did not exist before:
    // `T*` becomes `i32*`, and the table is where composed types live.
    Type_table& table()
    {
        return table_;
    }

    // Set for every file-scope variable that has an initialiser, and for nothing else - the rule
    // requires one to be a constant expression, so failing to fold one is an internal error.
    std::optional<Constant_value> constant_of( Node_id node ) const
    {
        const auto found = constants_.find( node.v );

        return found == constants_.end() ? std::nullopt : std::optional<Constant_value>( found->second );
    }

    // The callable a call resolved to - a method, a constructor, or one function out of an overload
    // set. Carried rather than looked up again: choosing a callable by name and argument types is a
    // *rule*, and lowering repeating it is how the two would drift the day that rule grows.
    Node_id callee_of( Node_id call ) const
    {
        const auto found = callees_.find( call.v );

        return found == callees_.end() ? Node_id {} : found->second;
    }

    const std::vector<Node_id>& struct_order() const
    {
        return struct_order_;
    }

    // Deduplicated: two calls at the same type arguments are one instantiation, because one is all
    // that will be emitted.
    const std::vector<Instantiation>& instantiations() const
    {
        return instantiations_;
    }

    // Every generic-to-generic call in the program. What a call site wrote is only the seed: an
    // instance of `f<i32>` needs `g<i32>` emitted too, and no call site ever named that.
    const std::vector<Generic_call>& generic_calls() const
    {
        return generic_calls_;
    }

    // D2: a type owns when it has a destructor, directly or through a by-value member. Recorded
    // rather than recomputed because drop elaboration runs on KIR, after the checker has gone.
    // Keyed by Type_id because that is what every caller holds; the set below stores declarations.
    bool is_owning( Type_id type ) const
    {
        if( !type.is_valid() || !table_.is_struct( type ) )
        {
            return false;
        }

        const Node_id decl = table_.get( type ).declaration;

        return decl.is_valid() && owning_.contains( decl.v );
    }

    // Which instantiation a call resolved to, or nothing for an ordinary call.
    std::optional<std::size_t> instantiation_of( Node_id call ) const
    {
        const auto found = instantiation_of_.find( call.v );

        return found == instantiation_of_.end() ? std::nullopt : std::optional<std::size_t>( found->second );
    }

private:
    Type_table                              table_;
    std::vector<Type_id>                    types_;
    std::vector<Node_id>                    struct_order_;
    std::unordered_map<u32, Constant_value> constants_; // dependencies first, from the DFS post-order
    std::unordered_map<u32, Node_id>        callees_;   // Call_expr -> the callable it resolved to
    std::unordered_set<u32>                 owning_;
    std::vector<Instantiation>              instantiations_;
    std::unordered_map<u32, u32>            instantiation_of_;
    std::vector<Generic_call>               generic_calls_;
};

Types type_check( const Ast&, const Resolution&, const Literal_pool&, const Source_manager&, const Interner&, Diagnostics& );

// What a parameter is actually passed as: its own type, except a `ref` binding, which travels as
// an address. Shared because lowering, the prototype and the mangled name must all agree.
bool is_ref_parameter( const Ast& ast, Node_id param );

// The type parameters of a declaration, without the `where` clauses that share their list. Four
// places want exactly this and three of them were counting the clauses.
std::vector<Node_id> type_parameters( const Ast& ast, Node_id decl );

// Whether a declaration carries type parameters. The kind is checked first: only a function-like
// declaration has a fixed fourth slot, and a call's callee may be an aggregate, whose children are
// its members.
bool is_generic( const Ast& ast, Node_id decl );

// An FFI declaration: a function with no body. `extern` is the only rule that produces one (D18's
// corollary makes a bare prototype an error), so the absent body is the marker and there is no flag.
bool is_extern( const Ast& ast, Node_id decl );

// Whether `member` may be named from inside the aggregate `from`, which is `enclosing_aggregate` of
// whatever function the access was written in - and an invalid id when that was a free function.
bool is_visible_from( const Ast& ast, Node_id member, Node_id from );

// Whether parameter 0 is the synthesised `this`: true of a method, constructor and destructor,
// false of a free function and of M7's static method. The node kind cannot answer it any more.
bool has_receiver( const Ast& ast, Node_id decl );

// A member that belongs to the type rather than to an object (PLAN §12, M7).
bool is_static_method( const Ast& ast, Node_id method );

// The mode a declaration was written with, or Keyword::Count for none. The one reader of a
// Mode_type's aux: three separate copies of this test existed before it, and a fourth was about to.
// D7: whether any variant carries a payload. That one answer decides the representation - a
// payload-free enum is its underlying integer, and one with payloads is a struct holding a tag and
// every payload field. Shared because lowering and the emitter must agree, and disagreeing would
// mean writing a tag into something that has none.
bool enum_has_payload( const Ast& ast, Node_id enum_decl );

Keyword parameter_mode( const Ast& ast, Node_id decl );

bool is_borrowed_binding( const Ast& ast, const Types& types, Node_id decl );
bool is_const_binding( const Ast& ast, Node_id decl );

// A method written with a trailing `const`, which is a `const ref` receiver. Its own name because
// the question is asked of the *method* while the answer lives on its parameter 0.
bool is_const_method( const Ast& ast, Node_id method );
// See the definitions: an aggregate's type-parameter bindings, and a field's type seen through
// them. Free rather than Checker members because the emitter needs the same answers and has no
// checker - the alternative being the rule written twice.
// Whether a declaration declares a destructor of its own. Free because three passes and the
// emitter all ask it, and only one of them has a checker.
bool has_destructor( const Ast& ast, Node_id declaration );
// D2 asked of an *instance*: `Box<i32>` and `Box<Buffer>` are two answers from one declaration, and
// a drop is elaborated against this one. Types::is_owning answers from the declaration instead,
// which is the open form's answer and the right one for the checker's move rules inside a generic
// body - the two are different questions and both are wanted.
bool     instance_owns( const Ast& ast, Type_table& table, Type_id instance, std::span<const Type_id> recorded );
Bindings aggregate_bindings( const Ast& ast, const Type_table& table, Type_id aggregate, std::span<const Type_id> recorded );
Type_id  field_type( const Ast& ast, Type_table& table, Type_id aggregate, Node_id field, std::span<const Type_id> recorded );

Type_id binding_type( const Ast& ast, const Types& types, Node_id decl );
// `const ref T` wraps the mode: Const_type( Mode_type( T ) ). Every question about a mode goes
// through here, so adding the spelling cannot quietly turn a `const ref` into a bare parameter.
Node_id unwrap_const( const Ast& ast, Node_id annotation );

} // namespace keel
