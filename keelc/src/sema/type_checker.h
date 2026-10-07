// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
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

// A generic named with concrete type arguments at some call site. The seed for the
// monomorphisation worklist: recorded by the checker because that is the only pass that resolves a
// type argument to a type.
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
    std::vector<Instantiation>              instantiations_;
    std::unordered_map<u32, u32>            instantiation_of_;
    std::vector<Generic_call>               generic_calls_;
};

Types type_check( const Ast&, const Resolution&, const Literal_pool&, const Source_manager&, const Interner&, Diagnostics& );

// A mode's two spellings: a `Param_mode` in a type, and the `Keyword` a call site writes. Bare and `const ref` share the
// empty marker, since after either the caller's variable is unchanged (D31), but stay two modes, so neither maps back.
Param_mode parameter_mode_of( const Ast& ast, Node_id param );
Keyword    call_marker_of( Param_mode mode );

bool is_borrowed_binding( const Ast& ast, const Types& types, Node_id decl );
// instance_owns for a type that may still hold a parameter, which `parameter_owns` answers
bool may_own(
    const Ast&                            ast,
    Type_table&                           table,
    Type_id                               instance,
    std::span<const Type_id>              recorded,
    std::vector<Type_id>&                 visiting,
    const std::function<bool( Type_id )>& parameter_owns
);
// D2 asked of an *instance*: `Box<i32>` and `Box<Buffer>` are two answers from one declaration, and
// a drop is elaborated against this one.
bool instance_owns( const Ast& ast, Type_table& table, Type_id instance, std::span<const Type_id> recorded );
// An aggregate's type-parameter bindings, and a field's type seen through them. Free because the
// emitter needs the same answers and has no checker.
Bindings aggregate_bindings( const Ast& ast, const Type_table& table, Type_id aggregate, std::span<const Type_id> recorded );
Type_id  field_type( const Ast& ast, Type_table& table, Type_id aggregate, Node_id field, std::span<const Type_id> recorded );

Type_id binding_type( const Ast& ast, const Types& types, Node_id decl );

} // namespace keel
