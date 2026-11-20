// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ast/node.h"
#include "common/types.h"
#include "sema/prelude_items.h"

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
    Run,          // T[]
    Array,        // T[N]
    Struct,       // `declaration` says which one
    Enum,
    Parameter, // a generic type parameter.
    Function,
    Field,
    Union,     // `A | B`; `arguments` are the members, sorted
    Interface, // `declaration` is the Interface_decl; reached only through a borrow or a pointer
    Size,      // a `u64` extent: `count` is a constant's value, `width` a Size_op over `arguments`
    Closure,   // a lambda's own type: `declaration` the Lambda_expr, `element` its `fn` signature,
               // `arguments` the enclosing generic's parameters
    Pack,      // what a size pack `u64... S` binds: `arguments` the sizes, spliced into a type's arguments
};

// Size_op::Constant is 0, so a constant size's `width` is 0.
enum class Size_op : u8
{
    Constant,
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Dynamic, // `dyn`, a size known only at run time; no operands
    // Shape functions over a pack. Product is a Size; the rest a Pack whose `arguments` are the
    // operands, the axis first, until substitute() computes them.
    Product,   // product(S)
    Broadcast, // broadcast(S, U)
    Dropped,   // dropped<A>(S)
    Inserted,  // inserted<A>(S)
    Reversed,  // reversed(S)
};

inline bool is_arithmetic( Size_op op )
{
    return op >= Size_op::Add && op <= Size_op::Remainder;
}

// What a shape function does wrong when it has no value: "drops an axis its sizes do not have".
std::string_view shape_failure( Size_op op );

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
    u64        count         = 0;     // Array: the number of elements, or 0 while its extent is not a constant
    bool       raw           = false; // Array: `T[?N]`, slots no drop or conversion treats as live
    bool       pack          = false; // Parameter: a size pack, `u64... S`
};

bool is_builtin_type_name( std::string_view spelling );
bool is_number_type_name( std::string_view spelling ); // an integer or a float

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
    Type_id run_of( Type_id element );
    Type_id array_of( Type_id element, u64 count, bool raw = false );
    Type_id array_of( Type_id element, Type_id extent, bool raw = false ); // the extent, a Size, is `arguments[0]`
    Type_id size( u64 value );
    Type_id dyn() const; // the one run-time size
    // Interned by its sizes. A pack parameter inside it is not spliced here: substitute() does that.
    Type_id pack( std::span<const Type_id> sizes );
    // The pack of `sizes` if one is interned, else invalid. Every aggregate instance ending in a
    // pack interns its tail, so deduction against an instance finds it without interning.
    Type_id interned_pack( std::span<const Type_id> sizes ) const;
    // Two constants fold; an overflow or a division by 0 is the Error type.
    Type_id size_op( Size_op op, Type_id lhs, Type_id rhs );
    // A shape function: computed when its operands are, else kept open; error when it fails.
    Type_id shape_op( Size_op op, std::span<const Type_id> operands );
    // The innermost shape function in `type` that fails under `bindings`, or invalid.
    Type_id failing_shape( Type_id type, const Bindings& bindings );
    Type_id function( Type_id return_type, std::span<const Parameter> parameters, Param_mode return_mode = Param_mode::Value );
    Type_id field( Type_id aggregate, Type_id member ); // `field( C ) -> T`: C in arguments, T in element
    Type_id enumeration( Node_id declaration, std::span<const Type_id> arguments, std::string_view name, Type_id underlying );

    // Interned by *declaration* and by its type arguments, never by name: two modules each
    // declaring `Point` must be two distinct types, and `Box<i32>` and `Box<f64>` must be two more.
    // The name is passed in because the table has no Interner of its own; the `<...>` part is
    // composed here, from the arguments' own names, so one place decides how a generic type reads.
    Type_id structure( Node_id declaration, std::span<const Type_id> arguments, std::string_view name );

    // Interned by declaration and arguments. `name` is the caller's (`fn[] at 3:5`); arguments are not shown.
    // `site` is the declaration's offset in its file, which with its module names it in C (`count`).
    Type_id
    closure( Node_id declaration, Type_id signature, std::span<const Type_id> arguments, std::string_view name, u64 site );

    // A type parameter, before any instantiation substitutes it away. Keyed by its Type_param_decl,
    // so `T` in one declaration is never `T` in another - the resolver already scoped them apart.
    Type_id parameter( Node_id declaration, std::string_view name );
    // `u64 N`: a parameter whose `element` is its declared value type.
    Type_id size_parameter( Node_id declaration, std::string_view name, Type_id value_type );
    // `u64... S`: a size parameter binding a Pack.
    Type_id size_pack_parameter( Node_id declaration, std::string_view name, Type_id value_type );

    // One per instance of an Interface_decl. Kept apart from the composites, since no C struct is emitted for one.
    Type_id interface( Node_id declaration, std::span<const Type_id> arguments, std::string_view name );

    // An interface's `self`: a parameter keyed by the interface's own declaration, which a
    // conformance or a bound substitutes away.
    Type_id self_of( Node_id interface );

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
    // Whether one binding of their parameters, free and distinct, makes `a` be `b` and `c` be `d`.
    bool unifies( Type_id a, Type_id b, Type_id c, Type_id d ) const;

    const Type&      get( Type_id id ) const;
    std::string_view name( Type_id id ) const; // "i32", "u8*" - for diagnostics

    // The declared name without its type arguments: `Box` for `Box<i32>`, and the whole name for
    // everything else. Exact because a declared name is an identifier and can hold no bracket.
    std::string_view base_name( Type_id id ) const;
    // Every composite type the table has interned, in a deterministic order. Includes the open form
    // of a generic aggregate - `Box<T>` - which a caller wanting only instances filters out with
    // mentions_parameter.
    std::vector<Type_id> composite_types() const;
    std::vector<Type_id> interface_types() const; // in interning order; no open form, which has no vtable
    std::vector<Type_id> run_types() const;       // in interning order
    std::vector<Type_id> array_types() const;     // in interning order
    std::vector<Type_id> closure_types() const;   // in interning order
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
    bool is_run( Type_id id ) const;
    bool is_array( Type_id id ) const;
    bool is_sized_array( Type_id id ) const; // an array whose extent is a constant
    bool is_raw_array( Type_id id ) const;   // `T[?N]`
    bool is_live_array( Type_id id ) const;  // `T[N]`, every element live
    bool needs_no_value( Type_id id ) const; // `T[?N]`, or a `T[N]` of them: declaring it is its initialisation
    bool is_size( Type_id id ) const;
    bool is_size_parameter( Type_id id ) const; // a pack parameter is one too
    bool is_pack_parameter( Type_id id ) const;
    bool is_pack( Type_id id ) const;  // a pack of sizes; not a shape function's open result
    bool is_shape( Type_id id ) const; // a shape function giving a pack, still open
    bool is_dyn( Type_id id ) const;
    // Whether `arguments` end in a pack parameter, which takes the arguments the others leave.
    bool ends_in_pack( std::span<const Type_id> arguments ) const;
    bool points_to_const( Type_id id ) const;
    bool is_parameter( Type_id id ) const; // a `T`, before an instantiation substitutes it away
    bool is_function( Type_id id ) const;
    bool is_closure( Type_id id ) const;
    bool is_field( Type_id id ) const;
    bool is_never( Type_id id ) const;
    bool is_builtin( Type_id id ) const; // a number or `bool`: a value type no declaration owns
    // Whether a parameter appears anywhere inside, not only at the top: `T` and `T*` both do, `i32*`
    // does not. What separates a type an instance can be emitted at from one that is still a
    // template, and structural for the same reason substitute() is.
    bool mentions_parameter( Type_id id ) const;
    bool is_void( Type_id id ) const;

    // §6.4 assignment: does every value of `from` exist in `to`? Not towards an interface, which
    // asks what is declared: that is Conformance_model::holds.
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
    void             set_module( Node_id declaration, std::string module ); // `kl::ring`, for stable_name
    std::string_view module( Type_id id ) const;                            // empty when not set

    // The name a type has in every compile: name() with each declared type's module before it.
    std::string stable_name( Type_id id ) const;

    std::vector<Type_id> union_types() const; // in interning order
    // A member's tag, the same in every union holding it, so widening keeps it, and in every
    // compile, so objects agree: a hash of its stable name.
    u32  error_tag( Type_id member ) const;
    bool has_member( Type_id union_type, Type_id member ) const;
    // The member declared by `declaration`, or invalid: how a variant's path finds its instance.
    Type_id member_declared_by( Type_id union_type, Node_id declaration ) const;
    bool    is_union( Type_id id ) const;
    bool    is_interface( Type_id id ) const;
    // A `Shape*`, which C carries as the object's address beside the implementer's vtable.
    bool is_interface_pointer( Type_id id ) const;
    bool is_run_pointer( Type_id id ) const; // a `ref T[]` or `const ref T[]`

    // Whether substitute( pattern, bindings ) would be `actual`, without interning it.
    bool substitutes_to( Type_id pattern, const Bindings& bindings, Type_id actual ) const;

    // The open form's parameters, each bound to this instance's argument, for an interface, struct,
    // class, enum or closure. Empty for anything else or a non-generic.
    Bindings instance_bindings( Type_id instance ) const;
    // `parameters` against flat `arguments` by position; a pack parameter takes what the fixed ones leave.
    // A function of several packs keeps each whole, one argument apiece, as `keeps_packs` says.
    Bindings             bind( std::span<const Type_id> parameters, std::span<const Type_id> arguments );
    bool                 keeps_packs( std::span<const Type_id> parameters ) const; // more than one pack
    std::vector<Type_id> spliced( std::span<const Type_id> arguments, const Bindings& bindings, bool spread = true );
    std::vector<Type_id> flattened( std::span<const Type_id> arguments, bool spread = true ) const; // nothing substituted

    // A copy of the resolver's, for the conformance model and the emitter, which see only this table.
    void                 set_prelude( const Prelude_items& items );
    const Prelude_items& prelude() const;
    bool                 is_box( Type_id type ) const;
    bool                 is_prelude_interface( Type_id interface ) const; // equatable, comparable or hashable
    // A non-generic declaration's one type, or invalid when it has none yet.
    Type_id declared( Node_id declaration ) const;
    // A size's value once `bindings` are read through, or none while it is open or fails.
    std::optional<u64> evaluate( Type_id size, const Bindings& bindings ) const;
    // A pack's sizes, as evaluate() gives one.
    std::optional<std::vector<u64>> evaluate_pack( Type_id pack, const Bindings& bindings ) const;

private:
    Type_id                                 add( const Type& type, std::string_view name );
    std::string                             const_spelling( Type_id element, bool const_element ) const;
    static u8                               width_index( u8 width );
    bool                                    reaches_dyn( Type_id size, const Bindings& bindings ) const;
    Type_id                                 bound_value( Type_id parameter, const Bindings& bindings ) const;
    std::optional<std::span<const Type_id>> known_sizes( Type_id operand ) const;
    // Interns the tail of `arguments` that the pack of its declaration's open form `open` takes.
    void intern_tail( std::span<const Type_id> open, std::span<const Type_id> arguments );

    Type_id composite(
        Type_kind kind, Node_id declaration, std::span<const Type_id> arguments, std::string_view name, Type_id element
    );

    std::deque<Type> types_; // types_[0] reserved so Type_id{} is invalid

    Type_id error_, void_, bool_, never_, dyn_; // what the constructor made
    Type_id integers_[4][2];                    // [width index][0 = signed, 1 = unsigned]
    Type_id floats_[2];

    std::unordered_map<u32, Type_id>              pointers_;      // element.v * 2 + const -> pointer id
    std::unordered_map<u32, Type_id>              many_pointers_; // element.v * 2 + const -> many pointer id
    std::unordered_map<u32, Type_id>              runs_;          // element.v -> run id
    std::map<std::tuple<u32, u32, bool>, Type_id> arrays_;        // (element.v, extent.v, raw) -> array id
    std::unordered_map<u64, Type_id>              sizes_;         // value -> constant size
    std::map<std::tuple<u8, u32, u32>, Type_id>   size_ops_;      // (op, lhs.v, rhs.v) -> computed size
    std::map<std::vector<u32>, Type_id>           packs_;         // the sizes' ids -> pack
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
    std::unordered_map<u32, std::vector<Instance>> interfaces_; // Interface_decl node -> its instances
    std::unordered_map<u32, std::vector<Instance>> closures_;   // Lambda_expr node -> its instances
    std::unordered_map<u32, Type_id>               params_;     // Type_param_decl node -> type parameter
    std::deque<std::string>                        composed_;

    Prelude_items prelude_;

    // Backs Type::arguments. A deque for the reason composed_ is one: the views handed out have to
    // survive every later insertion.
    std::deque<std::vector<Type_id>>    arguments_;
    std::deque<std::vector<Param_mode>> modes_;

    std::unordered_map<std::string_view, Type_id>
        by_spelling_; // views into composed_ // "u8*" etc; name() returns views into these

    std::vector<Type_id> functions_;
    std::vector<Type_id> fields_;
    std::vector<Type_id> unions_; // every union type the table has interned, in interning order

    std::unordered_map<u32, std::string> packages_; // Node_id of a declaration -> the package it was declared in
    std::unordered_set<u32>              unshown_packages_;
    std::unordered_map<u32, std::string> modules_; // Node_id of a declaration -> its module
};

// The help under "expected `X`, but got `Y`", or empty. Only a few mismatches get one: everywhere
// else §6.4 does widen, so the mismatch is a mismatch and saying more would be saying it twice.
std::string mismatch_hint( const Type_table& table, Type_id expected, Type_id actual );

} // namespace keel
