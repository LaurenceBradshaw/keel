// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ast/node.h"
#include "common/types.h"

namespace keel
{

struct Type_id
{
    u32  v = 0;
    bool is_valid() const
    {
        return v != 0;
    };
    friend bool operator==( Type_id lhs, Type_id rhs ) = default;
};

enum class Type_kind : u8
{
    Error, // poison
    Void,
    Never,
    Bool,
    Int, // signedness is a field, not a kind
    Float,
    Pointer,
    Many_pointer, // T[*]
    Struct,       // `declaration` says which one
    Enum,
    Parameter, // a generic type parameter.
    Function,
    Field,
    Union, // `A | B`; `arguments` are the members, sorted
};

enum class Param_mode : u8
{
    Value,
    Ref,
    Const_ref,
    Out,
    Move
};

// How a signature writes the mode: "const ref ", or nothing for a value.
std::string param_spelling( Param_mode mode );

struct Parameter
{
    Type_id    type;
    Param_mode mode = Param_mode::Value;

    bool operator==( const Parameter& other ) const = default;
};

// Defaulted rather than bare, so a kind that does not use a field can leave it out of the aggregate
// initialiser - and so adding a field later does not break every existing one.
struct Type
{
    Type_kind kind        = Type_kind::Error;
    u8        width       = 0;     // 8/16/32/64 for Int, 32/64 for Float, 0 otherwise
    bool      is_signed   = false; // only for Int
    Type_id   element     = {};    // For Pointer and Enum and Function and Field
    Node_id   declaration = {};    // for Struct and Enum; the node that defines it

    // What a generic aggregate was instantiated at: `Box<i32>` holds one, `Box` itself holds its
    // own parameters, and everything else holds none. A view rather than a vector, into storage the
    // table owns - the same arrangement name() already uses, and what keeps Type cheap to copy.
    std::span<const Type_id>    arguments {};
    std::span<const Param_mode> modes {};

    // The return's own mode. Only `Value` and `Const_ref` can reach here: a function type spells
    // the one returning borrow the language has, and the rest are refused where they are written.
    Param_mode return_mode   = Param_mode::Value;
    bool       const_element = false; // Pointer and Many_pointer: the element is read-only through it.
};

bool is_builtin_type_name( std::string_view spelling );

// A type parameter bound to a concrete type, keyed by the parameter's own Type_id. Named because
// it travels from the checker's call site through to the lowerer's instantiation.
using Bindings = std::unordered_map<u32, Type_id>;

class Type_table
{
public:
    Type_table(); // interns the builtins; their ids are then stable for the run

    Type_id builtin( Type_kind kind ) const; // Error, Void, Bool
    Type_id integer( u8 width, bool is_signed ) const;
    Type_id floating( u8 width ) const;
    Type_id pointer_to( Type_id element, bool const_element = false ); // interns; same element -> same id
    Type_id many_pointer_to( Type_id element, bool const_element = false );
    Type_id function( Type_id return_type, std::span<const Parameter> parameters, Param_mode return_mode = Param_mode::Value );
    Type_id field( Type_id aggregate, Type_id member ); // `field( C ) -> T`: C in arguments, T in element
    Type_id enumeration( Node_id declaration, std::span<const Type_id> arguments, std::string_view name, Type_id underlying );

    // Interned by *declaration* and by its type arguments, never by name: two modules each
    // declaring `Point` must be two distinct types, and `Box<i32>` and `Box<f64>` must be two more.
    // The name is passed in because the table has no Interner of its own; the `<...>` part is
    // composed here, from the arguments' own names, so one place decides how a generic type reads.
    Type_id structure( Node_id declaration, std::span<const Type_id> arguments, std::string_view name );

    // A type parameter, before any instantiation substitutes it away. Keyed by its Type_param_decl,
    // so `T` in one declaration is never `T` in another - the resolver already scoped them apart.
    Type_id parameter( Node_id declaration, std::string_view name );

    // `T` -> `i32`, `T*` -> `i32*`. Structural, because a parameter can appear inside a type
    // constructor as readily as alone, and `T*` is reachable the moment anyone writes it.
    Type_id substitute( Type_id type, const Bindings& bindings );

    // §6.7: nested unions flatten and members sort by id and deduplicate, so `A | B` and `B | A` are
    // one type. A single member left is that member.
    Type_id error_union( std::span<const Type_id> members );

    // The inverse: `T` against an `i32` binds `T`, `Box<T>` against a `Box<i32>` binds it one level
    // down. Structural for the same reason substitute() is. False means the two do not match at
    // all, which is not an error here - the ordinary argument check reports that - so a caller
    // deduces into a scratch map and keeps it only when this returns true.
    bool deduce( Type_id pattern, Type_id actual, Bindings& into ) const;

    const Type&      get( Type_id id ) const;
    std::string_view name( Type_id id ) const; // "i32", "u8*" - for diagnostics

    // The declared name without its type arguments: `Box` for `Box<i32>`, and the whole name for
    // everything else. Exact because a declared name is an identifier and can hold no bracket.
    std::string_view base_name( Type_id id ) const;
    // Every composite type the table has interned, in a deterministic order. Includes the open form
    // of a generic aggregate - `Box<T>` - which a caller wanting only instances filters out with
    // mentions_parameter.
    std::vector<Type_id> composite_types() const;
    // Every function type interned, in interning order. Separate from composite_types because the
    // one consumer of that list wants types that become a C struct, and a function type becomes a
    // typedef instead.
    std::vector<Type_id> function_types() const;
    // The type a source spelling names, or invalid if it names none. Only the eleven a program may
    // actually write - not "<error>", and not composed pointer names, which reach sema as
    // Pointer_type nodes rather than as identifiers.
    Type_id from_spelling( std::string_view spelling ) const;

    bool is_error( Type_id id ) const; // absorbs: checked at the top of most checker branches
    bool references_error( Type_id id ) const;
    bool is_integer( Type_id id ) const;
    bool is_float( Type_id id ) const;
    bool is_struct( Type_id id ) const;
    bool is_enum( Type_id id ) const;
    bool is_pointer( Type_id id ) const;
    bool is_many_pointer( Type_id id ) const;
    bool points_to_const( Type_id id ) const;
    bool is_parameter( Type_id id ) const; // a `T`, before an instantiation substitutes it away
    bool is_function( Type_id id ) const;
    bool is_field( Type_id id ) const;
    bool is_never( Type_id id ) const;
    // Whether a parameter appears anywhere inside, not only at the top: `T` and `T*` both do, `i32*`
    // does not. What separates a type an instance can be emitted at from one that is still a
    // template, and structural for the same reason substitute() is.
    bool mentions_parameter( Type_id id ) const;
    bool is_void( Type_id id ) const;

    // §6.4 assignment: does every value of `from` exist in `to`?
    bool holds( Type_id from, Type_id to ) const;

    // Smallest type losslessly holding both, or invalid if none exist
    Type_id common( Type_id a, Type_id b ) const;

    // What C++ would produce, integral promotion included.
    Type_id cpp_result( Type_id a, Type_id b ) const;

    // §6.4 binary operators: the result, or invalid meaning "compile error"
    Type_id arithmetic_result( Type_id a, Type_id b ) const;

    // Does a literal value fit? Sign is separate because `-5` parses as a Unary_expr wrapping the
    // literal 5, so the magnitude always arrives unsigned and the range check happens after it.
    bool fits( u64 magnitude, bool negative, Type_id type ) const;

    // A float literal arrives as a value, not as a magnitude and a sign: negation cannot take a
    // float out of range, so there is no asymmetry to account for.
    bool fits_float( f64 value, Type_id type ) const;

    // What an unsuffixed literal becomes with no context to give it a type.
    Type_id default_integer() const;
    Type_id default_float() const;

    void             set_package( Node_id declaration, std::string_view package, bool shown = true );
    std::string_view package( Type_id id ) const;

    std::vector<Type_id> union_types() const; // in interning order
    // A member's tag, the same in every union holding it, so widening keeps it.
    u32  error_tag( Type_id member ) const;
    bool has_member( Type_id union_type, Type_id member ) const;
    // The member declared by `declaration`, or invalid: how a variant's path finds its instance.
    Type_id member_declared_by( Type_id union_type, Node_id declaration ) const;
    bool    is_union( Type_id id ) const;

private:
    Type_id     add( const Type& type, std::string_view name );
    std::string const_spelling( Type_id element, bool const_element ) const;
    static u8   width_index( u8 width );

    Type_id composite(
        Type_kind kind, Node_id declaration, std::span<const Type_id> arguments, std::string_view name, Type_id element
    );

    std::deque<Type> types_; // types_[0] reserved so Type_id{} is invalid

    Type_id error_, void_, bool_, never_; // what the constructor made
    Type_id integers_[4][2];              // [width index][0 = signed, 1 = unsigned]
    Type_id floats_[2];

    std::unordered_map<u32, Type_id> pointers_;      // element.v * 2 + const -> pointer id
    std::unordered_map<u32, Type_id> many_pointers_; // element.v * 2 + const -> many pointer id
    // Struct_decl node -> every instantiation of it. A list scanned linearly rather than a map
    // keyed on the arguments: a program has a handful of instantiations per generic, and hashing a
    // vector of Type_ids to avoid a handful of comparisons is not a trade worth making - the same
    // reading record_instantiation takes of the same question.
    struct Instance
    {
        std::span<const Type_id> arguments;
        Type_id                  type;
    };

    std::unordered_map<u32, std::vector<Instance>> composites_;
    std::unordered_map<u32, Type_id>               params_; // Type_param_decl node -> type parameter
    std::deque<std::string>                        composed_;

    // Backs Type::arguments. A deque for the reason composed_ is one: the views handed out have to
    // survive every later insertion.
    std::deque<std::vector<Type_id>>    arguments_;
    std::deque<std::vector<Param_mode>> modes_;

    std::unordered_map<std::string_view, Type_id>
        by_spelling_; // views into composed_ // "u8*" etc; name() returns views into these

    std::vector<Type_id>         functions_;
    std::vector<Type_id>         fields_;
    std::vector<Type_id>         unions_;     // every union type the table has interned, in interning order
    std::unordered_map<u32, u32> error_tags_; // member -> its tag

    std::unordered_map<u32, std::string> packages_; // Node_id of a declaration -> the package it was declared in
    std::unordered_set<u32>              unshown_packages_;
};

// The help under "expected `X`, but got `Y`", or empty. Only a few mismatches get one: everywhere
// else §6.4 does widen, so the mismatch is a mismatch and saying more would be saying it twice.
std::string mismatch_hint( const Type_table& table, Type_id expected, Type_id actual );

} // namespace keel
