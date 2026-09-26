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

    // `fn( i32 ) -> i32` is `F3i32E3i32`: the parameters between brackets, then the return. Its
    // own name is no identifier, so it cannot go through the length-prefixed path below - and `F`
    // is a letter no encoded type starts with, which is what keeps the scheme injective.
    if( types.is_function( id ) )
    {
        out += 'F';

        for( const Type_id parameter : types.get( id ).arguments )
        {
            encode_type( out, parameter, types );
        }

        out += 'E';
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

// The marker leads, and every letter it can be is one no encoded type starts with - a type starts
// with a digit or with `P`. `S` is the same trick for the type a member belongs to, which leads the
// whole list.
std::string construct_arg_string( std::span<const Mangled_parameter> params, const Type_table& types, Type_id enclosing = {} )
{
    std::string encoded;

    if( enclosing.is_valid() )
    {
        encoded += 'S';
        encode_type( encoded, enclosing, types );
    }

    for( const Mangled_parameter& param : params )
    {
        if( !encoded.empty() )
        {
            encoded += '_';
        }

        if( param.marker != '\0' )
        {
            encoded += param.marker;
        }

        encode_type( encoded, param.type, types );
    }

    return encoded;
}
} // namespace

std::string mangle_function(
    std::string_view                   module,
    std::string_view                   name,
    std::span<const Mangled_parameter> params,
    const Type_table&                  types,
    std::span<const Type_id>           type_arguments,
    Type_id                            enclosing
)
{
    // An instantiation carries both. The type arguments alone would collide between two overloads
    // of one generic at the same arguments, and the parameters alone between two instantiations -
    // and the parameters are the caller's substituted ones, so nothing here spells a `T`.
    if( !type_arguments.empty() )
    {
        return fmt::format(
            "kl_{}_{}__I{}E__{}",
            module,
            name,
            construct_arg_string( type_arguments, types ),
            construct_arg_string( params, types, enclosing )
        );
    }

    return fmt::format( "kl_{}_{}__{}", module, name, construct_arg_string( params, types, enclosing ) );
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

std::string mangle_function_type( std::string_view module, Type_id type, const Type_table& types )
{
    std::string encoded;

    encode_type( encoded, type, types );

    return fmt::format( "kl_{}_fn__{}", module, encoded );
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
    std::string_view                   module,
    std::string_view                   type_name,
    std::span<const Type_id>           type_arguments,
    std::span<const Mangled_parameter> params,
    const Type_table&                  types
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

namespace
{
// A plain by-value parameter, which is what almost every test below wants.
Mangled_parameter by_value( Type_id type )
{
    return Mangled_parameter { .type = type, .marker = '\0' };
}
} // namespace

TEST_CASE( "mangle_function_spells_the_signature", "[codegen][mangle]" )
{
    const Type_table table;

    const Mangled_parameter signed32  = by_value( table.integer( 32, true ) );
    const Mangled_parameter unsigned8 = by_value( table.integer( 8, false ) );

    std::vector<Mangled_parameter>       none;
    const std::vector<Mangled_parameter> two   = { signed32, signed32 };
    const std::vector<Mangled_parameter> mixed = { unsigned8, by_value( table.floating( 64 ) ) };

    // The module is empty until M8, which leaves the doubled underscore in place.
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

// The two collisions overloading makes reachable. Both were written down as harmless while a second
// declaration of a name was caught by name alone, and a second declaration of a name is exactly
// what overloading permits.
TEST_CASE( "mangle_tells_apart_what_a_call_site_tells_apart", "[codegen][mangle]" )
{
    Type_table table;

    const Type_id i32     = table.integer( 32, true );
    const Type_id pointer = table.pointer_to( i32 );

    SECTION( "a borrow and a pointer" )
    {
        // `void f( ref i32 n )` and `void f( i32* n )`: two parameters a call site distinguishes,
        // because one is written `f( ref x )` and the other `f( p )`.
        const Mangled_parameter borrowed[] = { { .type = i32, .marker = 'R' } };
        const Mangled_parameter pointed[]  = { by_value( pointer ) };

        REQUIRE( mangle_function( "", "f", borrowed, table ) == "kl__f__R3i32" );
        REQUIRE( mangle_function( "", "f", pointed, table ) == "kl__f__P3i32" );
        REQUIRE( mangle_function( "", "f", borrowed, table ) != mangle_function( "", "f", pointed, table ) );
    }

    SECTION( "a transfer and a copy" )
    {
        const Mangled_parameter moved[]  = { { .type = i32, .marker = 'M' } };
        const Mangled_parameter copied[] = { by_value( i32 ) };

        REQUIRE( mangle_function( "", "f", moved, table ) == "kl__f__M3i32" );
        REQUIRE( mangle_function( "", "f", moved, table ) != mangle_function( "", "f", copied, table ) );
    }

    SECTION( "two overloads of one generic at the same type arguments" )
    {
        // `void f<T>( T a )` and `void f<T>( T a, i32 b )` at `i32`. Named by their type arguments
        // alone they were one symbol; the parameters are the substituted ones, so neither spells a
        // `T`.
        const Mangled_parameter one[] = { by_value( i32 ) };
        const Mangled_parameter two[] = { by_value( i32 ), by_value( i32 ) };
        const Type_id           at[]  = { i32 };

        REQUIRE( mangle_function( "", "f", one, table, at ) == "kl__f__I3i32E__3i32" );
        REQUIRE( mangle_function( "", "f", two, table, at ) == "kl__f__I3i32E__3i32_3i32" );
        REQUIRE( mangle_function( "", "f", one, table, at ) != mangle_function( "", "f", two, table, at ) );
    }

    // The one pair that still shares an encoding, and the reason it is allowed to: a `const ref T`
    // takes no marker at the call, so it may not be declared beside a bare `T` in the first place.
    SECTION( "a read-only borrow spells like the value it stands for" )
    {
        const Mangled_parameter borrowed[] = { by_value( i32 ) };
        const Mangled_parameter plain[]    = { by_value( i32 ) };

        REQUIRE( mangle_function( "", "f", borrowed, table ) == mangle_function( "", "f", plain, table ) );
    }
}

// The collision the checker cannot see: a member and a free function of one name have the same
// parameters and the same markers, and differ only in how they are written at a call.
TEST_CASE( "mangle_tells_a_member_from_a_free_function", "[codegen][mangle]" )
{
    Type_table table;

    const Type_id i32 = table.integer( 32, true );
    const Type_id box = table.structure( Node_id { 3 }, {}, "Box" );

    const Mangled_parameter own[]      = { by_value( i32 ) };
    const Mangled_parameter passed[]   = { by_value( box ), by_value( i32 ) };
    const Mangled_parameter borrowed[] = { { .type = box, .marker = 'R' }, by_value( i32 ) };

    SECTION( "a method and the two free spellings of it are three names" )
    {
        // `i32 at( i32 ) const` on `Box`, then `i32 at( Box, i32 )` and `i32 at( ref Box, i32 )`.
        // A const receiver is a `const ref Box` and takes no marker, so it encoded as the first of
        // these; a plain one is a `ref Box`, so it encoded as the second.
        REQUIRE( mangle_function( "", "at", own, table, {}, box ) == "kl__at__S3Box_3i32" );
        REQUIRE( mangle_function( "", "at", passed, table ) == "kl__at__3Box_3i32" );
        REQUIRE( mangle_function( "", "at", borrowed, table ) == "kl__at__R3Box_3i32" );
    }

    SECTION( "the tag names which type, not merely that there is one" )
    {
        const Type_id cell = table.structure( Node_id { 5 }, {}, "Cell" );

        REQUIRE( mangle_function( "", "at", own, table, {}, cell ) == "kl__at__S4Cell_3i32" );
        REQUIRE( mangle_function( "", "at", own, table, {}, box ) != mangle_function( "", "at", own, table, {}, cell ) );
    }

    SECTION( "two instances of one generic" )
    {
        // The receiver used to carry this and is no longer among the parameters, so the tag is the
        // encoded type rather than the bare name - `3BoxI3i32E`, not `Box`.
        const Type_id of_i32 = table.structure( Node_id { 11 }, std::array { i32 }, "Box" );
        const Type_id of_f64 = table.structure( Node_id { 11 }, std::array { table.floating( 64 ) }, "Box" );

        REQUIRE( mangle_function( "", "at", own, table, {}, of_i32 ) == "kl__at__S3BoxI3i32E_3i32" );
        REQUIRE( mangle_function( "", "at", own, table, {}, of_i32 ) != mangle_function( "", "at", own, table, {}, of_f64 ) );
    }

    SECTION( "a member with no parameters of its own" )
    {
        const std::vector<Mangled_parameter> none;

        REQUIRE( mangle_function( "", "area", none, table, {}, box ) == "kl__area__S3Box" );
        REQUIRE( mangle_function( "", "area", none, table, {}, box ) != mangle_function( "", "area", none, table ) );
    }

    // M7's shape: a static method has no receiver for a marker to sit on, which is why the tag is
    // the name's rather than parameter 0's - it survives the receiver not existing.
    SECTION( "a member whose own parameters include its type" )
    {
        REQUIRE( mangle_function( "", "at", passed, table, {}, box ) == "kl__at__S3Box_3Box_3i32" );
        REQUIRE( mangle_function( "", "at", passed, table, {}, box ) != mangle_function( "", "at", passed, table ) );
    }

    SECTION( "an instantiated member carries both" )
    {
        const Type_id at[] = { i32 };

        REQUIRE( mangle_function( "", "at", own, table, at, box ) == "kl__at__I3i32E__S3Box_3i32" );
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
        const Mangled_parameter as_pointer[] = { by_value( pointer ) };
        const Mangled_parameter as_struct[]  = { by_value( lookalike ) };

        REQUIRE( mangle_function( "", "f", as_pointer, table ) != mangle_function( "", "f", as_struct, table ) );
        REQUIRE( mangle_function( "", "f", as_pointer, table ) == "kl__f__P3i32" );
        REQUIRE( mangle_function( "", "f", as_struct, table ) == "kl__f__4i32p" );
    }

    SECTION( "two parameters cannot be read as one" )
    {
        // `u8` and `8` next to each other would run together without the lengths, and the
        // separator is for the reader rather than for the parse.
        const Mangled_parameter two[] = { by_value( table.integer( 8, false ) ), by_value( table.integer( 8, true ) ) };

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

    SECTION( "an instantiation carries its type arguments and its substituted parameters" )
    {
        // The parameters are substituted before they get here, so `kl__id__T__3i32` - which reads
        // as though `T` were a type someone could write - is not a form this can produce.
        const Mangled_parameter substituted[] = { by_value( i32 ) };
        const Type_id           arguments[]   = { i32 };

        REQUIRE( mangle_function( "", "id", substituted, table, arguments ) == "kl__id__I3i32E__3i32" );
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
