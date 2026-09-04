#pragma once
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
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
    Bool,
    Int, // signedness is a field, not a kind
    Float,
    Pointer, // M2; element unused before then
    Struct,  // `declaration` says which one
};

// Defaulted rather than bare, so a kind that does not use a field can leave it out of the aggregate
// initialiser - and so adding a field later does not break every existing one.
struct Type
{
    Type_kind kind        = Type_kind::Error;
    u8        width       = 0;     // 8/16/32/64 for Int, 32/64 for Float, 0 otherwise
    bool      is_signed   = false; // only for Int
    Type_id   element     = {};    // only for Pointer
    Node_id   declaration = {};    // only for Struct; the Struct_decl node that defines it
};

class Type_table
{
public:
    Type_table(); // interns the builtins; their ids are then stable for the run

    Type_id builtin( Type_kind kind ) const; // Error, Void, Bool
    Type_id integer( u8 width, bool is_signed ) const;
    Type_id floating( u8 width ) const;
    Type_id pointer_to( Type_id element ); // interns; same element -> same id

    // Interned by *declaration*, not by name: two modules each declaring `Point` must be two
    // distinct types. The name is passed in because the table has no Interner of its own.
    Type_id structure( Node_id declaration, std::string_view name );

    const Type&      get( Type_id id ) const;
    std::string_view name( Type_id id ) const; // "i32", "u8*" - for diagnostics
    // The type a source spelling names, or invalid if it names none. Only the eleven a program may
    // actually write - not "<error>", and not composed pointer names, which reach sema as
    // Pointer_type nodes rather than as identifiers.
    Type_id from_spelling( std::string_view spelling ) const;

    bool is_error( Type_id id ) const; // absorbs: checked at the top of most checker branches
    bool is_integer( Type_id id ) const;
    bool is_float( Type_id id ) const;
    bool is_struct( Type_id id ) const;

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

    // What an unsuffixed literal becomes with no context to give it a type.
    Type_id default_integer() const;
    Type_id default_float() const;

private:
    Type_id   add( const Type& type, std::string_view name );
    static u8 width_index( u8 width );

    std::deque<Type> types_; // types_[0] reserved so Type_id{} is invalid

    Type_id error_, void_, bool_; // what the constructor made
    Type_id integers_[4][2];      // [width index][0 = signed, 1 = unsigned]
    Type_id floats_[2];

    std::unordered_map<u32, Type_id> pointers_; // element id -> pointer id
    std::unordered_map<u32, Type_id> structs_;  // Struct_decl node -> struct type
    std::deque<std::string>          composed_;

    std::unordered_map<std::string_view, Type_id>
        by_spelling_; // views into composed_ // "u8*" etc; name() returns views into these
};

} // namespace keel
