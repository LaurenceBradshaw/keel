#include "codegen_c/spelling.h"
#include <cassert>
#include <vector>

#include <fmt/format.h>
#include "codegen_c/mangle.h"
#include "lex/token.h"

namespace keel
{

namespace
{
std::vector<Type_id> parameter_types_vector( const Ast& ast, const Types& types, Node_id decl )
{
    std::vector<Type_id> params;

    for( const Node_id param : ast.children( ast.children( decl )[1] ) )
    {
        // The type is recorded on the Param_decl itself, by declare_signatures - not on the type
        // annotation beneath it, which is never typed.
        params.push_back( types.type_of( param ) );
    }

    return params;
}
} // namespace

std::string Spelling::type( Type_id type ) const
{
    const Type& described = types.table().get( type );

    switch( described.kind )
    {
    case Type_kind::Void:
        return "void";

    case Type_kind::Bool:
        return "bool"; // <stdbool.h>'s, per §7.8

    case Type_kind::Int:
        // §7.8: exact-width types, never C's own, whose sizes are a platform question.
        return fmt::format( "{}int{}_t", described.is_signed ? "" : "u", described.width );

    case Type_kind::Float:
        return described.width == 32 ? "float" : "double";

    case Type_kind::Struct:
        return structure( described.declaration );

    case Type_kind::Pointer:
        return fmt::format( "{}*", this->type( described.element ) );

    default:
        assert( false && "no C spelling for this type" );
        return "void";
    }
}

std::string Spelling::structure( Node_id declaration ) const
{
    return fmt::format( "struct {}", mangle_struct( "", interner.text( Symbol_id { ast.aux( declaration ) } ) ) );
}

std::string Spelling::field( Node_id declaration ) const
{
    return mangle_local( interner.text( Symbol_id { ast.aux( declaration ) } ), declaration.v );
}

std::string Spelling::function( Node_id declaration ) const
{
    std::vector<Type_id> params = parameter_types_vector( ast, types, declaration );
    return mangle_function( "", interner.text( Symbol_id { ast.aux( declaration ) } ), params, types.table() );
}

std::string Spelling::parameter_types( Node_id declaration ) const
{

    std::vector<Type_id> params = parameter_types_vector( ast, types, declaration );

    std::string rendered;

    for( std::size_t i = 0; i < params.size(); ++i )
    {
        if( i != 0 )
        {
            rendered += ", ";
        }

        rendered += type( params[i] );
    }

    if( rendered.empty() )
    {
        rendered = "void"; // C11: an empty list is a prototype that says nothing about arity
    }

    return rendered;
}

std::string c_float( f64 value )
{
    std::string text = fmt::format( "{}", value );

    if( text.find( '.' ) == std::string::npos && text.find( 'e' ) == std::string::npos )
    {
        text += ".0";
    }

    return text;
}

std::string c_integer( u64 value )
{
    return value > 9223372036854775807ull ? fmt::format( "{}ull", value ) : fmt::format( "{}", value );
}

std::string Spelling::global_definition( Node_id declaration ) const
{
    const Type_id     variable = types.type_of( declaration );
    const std::string name     = mangle_local( interner.text( Symbol_id { ast.aux( declaration ) } ), declaration.v );

    // The checker evaluated the initialiser; this prints the value. It used to print the expression
    // instead, which meant a backend re-deriving §6.4's conversions in its own spelling of the tree
    // - and an LLVM backend would have had to translate that tree rather than emit a ConstantInt.
    const std::optional<Constant_value> value = types.constant_of( declaration );

    // No initialiser is zero, which C guarantees for file-scope storage - so nothing is written
    // rather than a zero invented here.
    if( !value )
    {
        return fmt::format( "{} {};", type( variable ), name );
    }

    const Type_table& table = types.table();

    std::string text;

    if( value->kind == Constant_value::Kind::Float )
    {
        text = c_float( value->floating );
    }
    else if( table.is_pointer( variable ) )
    {
        text = "NULL"; // the only pointer constant there is, and it folded to zero
    }
    else if( variable == table.builtin( Type_kind::Bool ) )
    {
        text = value->magnitude != 0 ? "true" : "false";
    }
    else
    {
        text = fmt::format( "{}{}", value->negative ? "-" : "", c_integer( value->magnitude ) );
    }

    return fmt::format( "{} {} = {};", type( variable ), name, text );
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{

// Both rules exist because C reads a literal differently from how fmt writes one. Tested directly
// rather than only through the goldens, because the boundaries are where they matter and no
// fixture happens to sit on them.
TEST_CASE( "c_float_stays_a_float", "[codegen][spelling]" )
{
    // The rule: a value that printed as an integer needs a point putting back, or C reads it as an
    // int and the whole expression changes type.
    REQUIRE( c_float( 1.0 ) == "1.0" );
    REQUIRE( c_float( -2.0 ) == "-2.0" );
    REQUIRE( c_float( 0.0 ) == "0.0" );

    // Already unambiguous, so left alone.
    REQUIRE( c_float( 1.5 ) == "1.5" );
    REQUIRE( c_float( 0.25 ) == "0.25" );

    // Exponent form is a float to C without help.
    REQUIRE( c_float( 1e30 ).find( 'e' ) != std::string::npos );
    REQUIRE( c_float( 1e30 ).find( ".0" ) == std::string::npos );
}

TEST_CASE( "c_integer_suffixes_what_c_would_narrow", "[codegen][spelling]" )
{
    REQUIRE( c_integer( 0 ) == "0" );
    REQUIRE( c_integer( 42 ) == "42" );

    // INT64_MAX still fits a signed type, so it needs nothing.
    REQUIRE( c_integer( 9223372036854775807ull ) == "9223372036854775807" );

    // One past it does: without the suffix C would pick a signed type too narrow to hold it.
    REQUIRE( c_integer( 9223372036854775808ull ) == "9223372036854775808ull" );
    REQUIRE( c_integer( 18446744073709551615ull ) == "18446744073709551615ull" );
}

} // namespace keel
#endif
