#include "codegen_c/mangle.h"
#include <fmt/format.h>
#include <string>

namespace keel
{

namespace
{
// One type, encoded so that no two distinct types encode alike.
//
// Every name is preceded by its length, which is what makes it injective: a struct genuinely called
// `i32p` encodes as `4i32p`, while `i32*` encodes as `P3i32`. The previous scheme spelled both
// `i32p` and said so in a comment - it was safe only while every type name was a plain word, and a
// generic aggregate ends that, since `Box<i32>` is not one.
//
// No separator is needed between consecutive types: a length is always followed by a non-digit,
// because a declared name is an identifier and cannot start with one.
void encode_type( std::string& out, Type_id id, const Type_table& types )
{
    if( types.is_pointer( id ) )
    {
        out += 'P';
        encode_type( out, types.get( id ).element, types );
        return;
    }

    // `Box<i32>` is `3BoxI3i32E`: the base, then its arguments between brackets that cannot appear
    // in a name. Nested and multi-argument forms fall out - `Box<Box<i32>>` is `3BoxI3BoxI3i32EE`.
    const std::string_view base = types.base_name( id );

    out += fmt::format( "{}{}", base.size(), base );

    const std::span<const Type_id> arguments = types.get( id ).arguments;

    if( arguments.empty() )
    {
        return;
    }

    out += 'I';

    for( std::size_t i = 0; i < arguments.size(); ++i )
    {
        if( i != 0 )
        {
            out += '_';
        }

        encode_type( out, arguments[i], types );
    }

    out += 'E';
}

std::string construct_arg_string( std::span<const Type_id> params, const Type_table& types )
{
    std::string encoded;

    // Redundant to the lengths, which already separate these unambiguously - kept because
    // `3i32_3i32` is something a reader can take apart and `3i323i32` is not.
    for( const Type_id id : params )
    {
        if( !encoded.empty() )
        {
            encoded += '_';
        }

        encode_type( encoded, id, types );
    }

    return encoded;
}
} // namespace

std::string mangle_function(
    std::string_view         module,
    std::string_view         name,
    std::span<const Type_id> params,
    const Type_table&        types,
    std::span<const Type_id> type_arguments
)
{
    // An instantiation is named by its *type* arguments and nothing else. Its value parameters are
    // written in the declaration's parameters - `T` - so including them gave `kl__id__3i32`,
    // which is unique and reads as though `T` were a type someone could write. Two instantiations
    // of one generic differ only by type arguments, and two generics cannot share a name, so the
    // type arguments alone tell every instance apart.
    if( !type_arguments.empty() )
    {
        return fmt::format( "kl_{}_{}__{}", module, name, construct_arg_string( type_arguments, types ) );
    }

    return fmt::format( "kl_{}_{}__{}", module, name, construct_arg_string( params, types ) );
}

std::string mangle_struct( std::string_view module, Type_id type, const Type_table& types )
{
    // `kl__Box__I3i32E` for `Box<i32>`, and the bare `kl__Point` for an aggregate with no type
    // arguments - so nothing a v0 program can write changes name.
    const std::string_view         base      = types.base_name( type );
    const std::span<const Type_id> arguments = types.get( type ).arguments;

    if( arguments.empty() )
    {
        return fmt::format( "kl_{}_{}", module, base );
    }

    return fmt::format( "kl_{}_{}__I{}E", module, base, construct_arg_string( arguments, types ) );
}

std::string mangle_destructor(
    std::string_view module, std::string_view type_name, std::span<const Type_id> type_arguments, const Type_table& types
)
{
    if( type_arguments.empty() )
    {
        return fmt::format( "kl_{}_{}__dtor", module, type_name );
    }

    return fmt::format( "kl_{}_{}__I{}E__dtor", module, type_name, construct_arg_string( type_arguments, types ) );
}

std::string mangle_constructor(
    std::string_view         module,
    std::string_view         type_name,
    std::span<const Type_id> type_arguments,
    std::span<const Type_id> params,
    const Type_table&        types
)
{
    const std::string argtypes = construct_arg_string( params, types );

    if( type_arguments.empty() )
    {
        return fmt::format( "kl_{}_{}__{}__ctor", module, type_name, argtypes );
    }

    return fmt::format(
        "kl_{}_{}__I{}E__{}__ctor", module, type_name, construct_arg_string( type_arguments, types ), argtypes
    );
}

std::string mangle_local( std::string_view name, u32 declaration )
{
    // kl_<name>_<node id>
    return fmt::format( "kl_{}_{}", name, declaration );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

namespace keel
{

TEST_CASE( "mangle_function_spells_the_signature", "[codegen][mangle]" )
{
    const Type_table table;

    const Type_id              signed32  = table.integer( 32, true );
    const Type_id              unsigned8 = table.integer( 8, false );
    std::vector<Type_id>       none;
    const std::vector<Type_id> two   = { signed32, signed32 };
    const std::vector<Type_id> mixed = { unsigned8, table.floating( 64 ) };

    // The module is empty until M7, which leaves the doubled underscore in place.
    REQUIRE( mangle_function( "", "main", none, table ) == "kl__main__" );
    REQUIRE( mangle_function( "", "add", two, table ) == "kl__add__3i32_3i32" );
    REQUIRE( mangle_function( "", "f", mixed, table ) == "kl__f__2u8_3f64" );
    REQUIRE( mangle_function( "math", "abs", { &signed32, 1 }, table ) == "kl_math_abs__3i32" );

    SECTION( "the parameter types are what make two names differ" )
    {
        REQUIRE( mangle_function( "", "f", { &signed32, 1 }, table ) != mangle_function( "", "f", { &unsigned8, 1 }, table ) );
        REQUIRE( mangle_function( "", "f", none, table ) != mangle_function( "", "f", { &signed32, 1 }, table ) );
    }

    // §7.5's reason for existing: a user function called `while` or `printf` must not become one.
    SECTION( "every name is prefixed, so none can collide with C" )
    {
        for( const std::string_view name : { "main", "while", "printf", "int", "malloc" } )
        {
            INFO( name );
            REQUIRE( mangle_function( "", name, none, table ).starts_with( "kl_" ) );
        }
    }
}

// The property the length prefixes exist for. The previous scheme spelled a type's name with `*`
// rewritten to `p`, and said in its own comment that it was safe only while every type name was a
// plain word - a generic aggregate ends that, and a struct may be called anything L15 allows.
TEST_CASE( "mangle_encodes_every_type_injectively", "[codegen][mangle]" )
{
    Type_table table;

    const Type_id i32     = table.integer( 32, true );
    const Type_id pointer = table.pointer_to( i32 );

    // A struct genuinely named `i32p`, which is what the old scheme spelled `i32*` as.
    const Type_id lookalike = table.structure( Node_id { 7 }, {}, "i32p" );

    SECTION( "a pointer and a struct that spells like one are different" )
    {
        const Type_id as_pointer[] = { pointer };
        const Type_id as_struct[]  = { lookalike };

        REQUIRE( mangle_function( "", "f", as_pointer, table ) != mangle_function( "", "f", as_struct, table ) );
        REQUIRE( mangle_function( "", "f", as_pointer, table ) == "kl__f__P3i32" );
        REQUIRE( mangle_function( "", "f", as_struct, table ) == "kl__f__4i32p" );
    }

    SECTION( "two parameters cannot be read as one" )
    {
        // `u8` and `8` next to each other would run together without the lengths, and the
        // separator is for the reader rather than for the parse.
        const Type_id two[] = { table.integer( 8, false ), table.integer( 8, true ) };

        REQUIRE( mangle_function( "", "f", two, table ) == "kl__f__2u8_2i8" );
    }

    SECTION( "a generic aggregate carries its arguments" )
    {
        const Type_id of_i32 = table.structure( Node_id { 11 }, std::array { i32 }, "Box" );
        const Type_id of_f64 = table.structure( Node_id { 11 }, std::array { table.floating( 64 ) }, "Box" );

        REQUIRE( mangle_struct( "", of_i32, table ) == "kl__Box__I3i32E" );
        REQUIRE( mangle_struct( "", of_f64, table ) == "kl__Box__I3f64E" );
        REQUIRE( mangle_struct( "", of_i32, table ) != mangle_struct( "", of_f64, table ) );
    }

    SECTION( "nested, and with several arguments" )
    {
        const Type_id of_i32  = table.structure( Node_id { 11 }, std::array { i32 }, "Box" );
        const Type_id nested  = table.structure( Node_id { 11 }, std::array { of_i32 }, "Box" );
        const Type_id several = table.structure( Node_id { 13 }, std::array { i32, table.floating( 64 ) }, "Pair" );

        REQUIRE( mangle_struct( "", nested, table ) == "kl__Box__I3BoxI3i32EE" );
        REQUIRE( mangle_struct( "", several, table ) == "kl__Pair__I3i32_3f64E" );
    }

    SECTION( "a non-generic aggregate is spelled exactly as before" )
    {
        const Type_id point = table.structure( Node_id { 17 }, {}, "Point" );

        REQUIRE( mangle_struct( "", point, table ) == "kl__Point" );
    }

    SECTION( "an instantiation is named by its type arguments and nothing else" )
    {
        // Its value parameters are written in the declaration's own parameters - `T` - so including
        // them gave `kl__id__T__i32`, which reads as though `T` were a type someone could write.
        const Type_id declared[]  = { table.parameter( Node_id { 19 }, "T" ) };
        const Type_id arguments[] = { i32 };

        REQUIRE( mangle_function( "", "id", declared, table, arguments ) == "kl__id__3i32" );
    }
}

TEST_CASE( "mangle_local_cannot_collide", "[codegen][mangle]" )
{
    REQUIRE( mangle_local( "x", 42 ).starts_with( "kl_" ) );

    SECTION( "two declarations of the same name are distinct" )
    {
        REQUIRE( mangle_local( "x", 1 ) != mangle_local( "x", 2 ) );
    }

    // The reason the node id is in there: both of these are legal Keel identifiers, and without
    // it they would become the same C name.
    SECTION( "`x` and `kl_x` stay distinct" )
    {
        REQUIRE( mangle_local( "x", 1 ) != mangle_local( "kl_x", 2 ) );
    }
}

} // namespace keel
#endif
