// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <string>
#include <string_view>
#include "ast/ast.h"
#include "common/types.h"
#include "lex/token.h"
#include "sema/aggregates.h"
#include "sema/reporter.h"
#include "sema/types_builder.h"

namespace keel::sema
{

// D40. What only the compiler can answer of a `T`; equality, order and hashing are prelude
// interfaces. One bit each, written out: the operators below mask with these values directly, so an
// ordinal here would make `copyable` the empty set.
enum class Bound : u8
{
    Copyable = 1u << 0, // trivially copyable - D29's `struct`, and what D31 needs to permit a copy
    Numeric  = 1u << 1,
    Integral = 1u << 2,
    Floating = 1u << 3,
};

using Bound_set = u8;

// Setting a bit is the only mutation a set has.
constexpr Bound_set operator|=( Bound_set& bits, Bound bound )
{
    bits |= static_cast<Bound_set>( bound );

    return bits;
}

// And reading one is the only question. A set is a `u8`, not a table lookup, so this asks nothing
// of `Bounds` and belongs beside the set rather than on the class that happens to own one.
constexpr bool contains( Bound_set bits, Bound bound )
{
    return ( bits & static_cast<Bound_set>( bound ) ) != 0;
}

std::string      known_bound_names();
std::string_view name_of_bound( Bound bound );
std::string_view bound_requirement( Bound bound );

std::optional<Bound> bound_for_name( std::string_view spelling );
std::optional<Bound> operator_to_bound( Token_kind kind );

// `cast` preserves the value and `wrap` keeps the low bits, so a pair of type kinds may permit one,
// both or neither. They are not one operator with two spellings.
enum class Conversion : u8
{
    None,
    Cast_only, // modular arithmetic has no meaning here
    Both,
    Unsafe, // a real conversion, but one that needs a gate Keel does not have yet
};

// Owns what each type parameter promised, and answers both halves of "may this type do that" -
// what a parameter promises, and what a concrete type delivers. `satisfies` is the one to ask when
// the answer has to cover either; Aggregates says which aggregates are finite enough to walk.
class Bounds
{
public:
    Bounds(
        const Ast&          ast,
        const Interner&     interner,
        const Literal_pool& literals,
        Types_builder&      types,
        Aggregates&         aggregates,
        Reporter&           reporter
    )
        : ast_( ast ),
          interner_( interner ),
          literal_pool_( literals ),
          types_( types ),
          table_( types.table() ),
          aggregates_( aggregates ),
          reporter_( reporter )
    {
    }

    bool has_bound( Type_id type, Bound bound ) const; // Parameter -> its set; else false
    bool satisfies( Type_id type, Bound bound ) const; // the same question, of a concrete type
    void check_bounds( Node_id parameter, Span at, Type_id argument, std::string_view callee, const Bindings& arguments );
    // The `copyable` checks that came before the struct order, which is what answers them.
    void check_deferred();

    // The bound set recorded for a parameter, or an empty set when it carries no `where` clause.
    Bound_set bounds_for( Type_id parameter ) const;

    // The interfaces a parameter's `where` clause names, in the order written, then what they require.
    std::span<const Type_id> interfaces_of( Type_id parameter ) const;
    bool                     promises( Type_id parameter, Type_id interface ) const;
    // A parameter's promise, a declared `implements`, or what the compiler supplies.
    bool conforms( Type_id type, Type_id interface ) const;
    // Whether a string literal becomes a `type`: only through the prelude's `from<str>`.
    bool adopts_text( Type_id type, Type_id text ) const;

    // The narrowest integer a parameter may turn out to be, for the checks that need a width. Zero
    // when no integer is admissible at all.
    u8 narrowest_admissible_width( Type_id parameter ) const;

    // Each answers over every type a parameter may turn out to be, and names the one that decided
    // it rather than the parameter - `i8` is an answer an author can act on, `T` is not. An invalid
    // answer means nothing had to be explained: no conversion witness, nothing narrowed, no overflow.
    Conversion conversion_between( Type_id from, Type_id to, Type_id& witness ) const;
    Type_id    narrowing_witness( Type_id from, Type_id to ) const;
    Type_id    type_the_literal_overflows( Node_id literal, bool negative, Bound_set bounds ) const;

    void declare_type_parameters( Node_id declaration, Node_id list );

private:
    // `integral` promises everything `numeric` does, and so on. Expanded once at the `where` clause
    // rather than walked at each query, which is what makes every later test one mask.
    Bound_set closure( Bound_set direct );
    // What a clause's interfaces require, closed over, so `where T : Shape` promises `Named` too.
    void require_refinements( std::vector<Type_id>& interfaces ) const;

    // How a forwarded parameter would keep a promise: a clause on the declaration that owns it.
    std::string forwarded_help( Type_id parameter, std::string_view promise ) const;
    // Why a type the compiler would make `interface` is not one: its own `equatable` replaced it.
    std::string dropped_help( Type_id type, Type_id interface ) const;

    void
    report_unmet( Node_id parameter, Span at, Type_id argument, std::string_view callee, Bound bound, std::string_view name );

    std::vector<Type_id> possible_types( Type_id type ) const;
    std::vector<Type_id> admissible_numeric_types( Bound_set bounds ) const;

    const Ast&          ast_;
    const Interner&     interner_;
    const Literal_pool& literal_pool_;
    Types_builder&      types_;
    Type_table&         table_;
    Aggregates&         aggregates_;
    Reporter&           reporter_;

    std::unordered_map<u32, Bound_set>            bounds_;     // Type_param_decl -> its closed bound set
    std::unordered_map<u32, std::vector<Type_id>> interfaces_; // Type_param_decl -> the interfaces it names
    std::unordered_map<u32, Node_id>              owner_of_;   // Type_param_decl -> the declaration that introduced it.

    struct Deferred
    {
        Node_id     parameter;
        Span        at;
        Type_id     argument;
        std::string callee;
    };
    std::vector<Deferred> deferred_;
};

} // namespace keel::sema
