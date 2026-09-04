#include "codegen_c/mangle.h"
#include <fmt/format.h>
#include <string>

namespace keel
{

std::string
mangle_function( std::string_view module, std::string_view name, std::span<const Type_id> params, const Type_table& types )
{
    // kl_<module>_<name>__<argtypes>

    std::string argtypes;
    for( const Type_id id : params )
    {
        if( !argtypes.empty() )
        {
            argtypes += '_';
        }

        argtypes += types.name( id );
    }

    return fmt::format( "kl_{}_{}__{}", module, name, argtypes );
}

std::string mangle_struct( std::string_view module, std::string_view name )
{
    return fmt::format( "kl_{}_{}", module, name );
}

std::string mangle_local( std::string_view name, u32 declaration )
{
    // kl_<name>_<node id>
    return fmt::format( "kl_{}_{}", name, declaration );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace keel
{

TEST_CASE( "mangle_function_spells_the_signature", "[codegen][mangle]" )
{
    const Type_table table;

    const Type_id              i32 = table.integer( 32, true );
    const Type_id              u8  = table.integer( 8, false );
    std::vector<Type_id>       none;
    const std::vector<Type_id> two   = { i32, i32 };
    const std::vector<Type_id> mixed = { u8, table.floating( 64 ) };

    // The module is empty until M7, which leaves the doubled underscore in place.
    REQUIRE( mangle_function( "", "main", none, table ) == "kl__main__" );
    REQUIRE( mangle_function( "", "add", two, table ) == "kl__add__i32_i32" );
    REQUIRE( mangle_function( "", "f", mixed, table ) == "kl__f__u8_f64" );
    REQUIRE( mangle_function( "math", "abs", { &i32, 1 }, table ) == "kl_math_abs__i32" );

    SECTION( "the parameter types are what make two names differ" )
    {
        REQUIRE( mangle_function( "", "f", { &i32, 1 }, table ) != mangle_function( "", "f", { &u8, 1 }, table ) );
        REQUIRE( mangle_function( "", "f", none, table ) != mangle_function( "", "f", { &i32, 1 }, table ) );
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
