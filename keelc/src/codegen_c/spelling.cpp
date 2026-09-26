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

    for( const Node_id param : ast.children( ast.child( decl, 1 ) ) )
    {
        // The type is recorded on the Param_decl itself, by the declaration pass - not on the type
        // annotation beneath it, which is never typed.
        params.push_back( binding_type( ast, types, param ) );
    }

    return params;
}
// What tells two overloads of one name apart: each parameter's *declared* type and the marker a
// call site writes for it. Not binding_type, which is the pointer a borrow travels as - that would
// spell `ref i32` and `i32*` alike, and they are two parameters a call site distinguishes.
std::vector<Mangled_parameter> mangled_parameters( const Ast& ast, const Types& types, Node_id decl )
{
    std::vector<Mangled_parameter> params;

    for( const Node_id param : ast.children( ast.child( decl, 1 ) ) )
    {
        const Keyword mode = is_const_binding( ast, param ) ? Keyword::Count : parameter_mode( ast, param );

        const char marker = mode == Keyword::Ref ? 'R' : mode == Keyword::Move ? 'M' : mode == Keyword::Out ? 'O' : '\0';

        params.push_back( Mangled_parameter { .type = types.type_of( param ), .marker = marker } );
    }

    return params;
}

// The declaration's type parameters against this instance's arguments. The same map the worklist
// builds when it emits the instance - written again here because a symbol is computed from the
// declaration and must come back through the instance, like everything else that reaches that way.
Bindings instance_bindings( const Ast& ast, const Types& types, Node_id declaration, std::span<const Type_id> type_arguments )
{
    const std::vector<Node_id> parameters = type_parameters( ast, ast.type_param_list( declaration ) );

    Bindings bindings;

    for( std::size_t i = 0; i < parameters.size() && i < type_arguments.size(); ++i )
    {
        bindings.emplace( types.type_of( parameters[i] ).v, type_arguments[i] );
    }

    return bindings;
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
        return structure( type );

    case Type_kind::Pointer:
        return fmt::format( "{}*", this->type( described.element ) );

    // The typedef's name, written out by the emitter before anything can be declared with it.
    case Type_kind::Function:
        return mangle_function_type( "", type, types.table() );

    // A payload-free enum is its underlying integer - not a C `enum`, whose type is
    // implementation-defined and which would buy nothing, since by here Keel has already erased the
    // distinction the tag existed for. One carrying payloads is a struct: a tag and the fields.
    case Type_kind::Enum:
        return enum_has_payload( ast, described.declaration ) ? structure( type ) : this->type( described.element );

    default:
        assert( false && "no C spelling for this type" );
        return "void";
    }
}

std::string Spelling::structure( Type_id type ) const
{
    return fmt::format( "struct {}", mangle_struct( "", type, types.table() ) );
}

std::string Spelling::field( Node_id declaration ) const
{
    return mangle_local( interner.text( Symbol_id { ast.aux( declaration ) } ), declaration.v );
}

std::string Spelling::function( Node_id declaration, std::span<const Type_id> type_arguments ) const
{
    // An extern names a symbol someone else defined, so the Keel name is the C name: `kl__malloc__u64`
    // would not link against anything.
    if( is_extern( ast, declaration ) )
    {
        return std::string( interner.text( Symbol_id { ast.aux( declaration ) } ) );
    }

    // A destructor's aux is the type's own name, and an instantiation is told apart by its type
    // arguments alone - the same rule mangle_function follows.
    if( ast.kind( declaration ) == Node_kind::Destructor_decl )
    {
        return mangle_destructor( "", interner.text( Symbol_id { ast.aux( declaration ) } ), type_arguments, types.table() );
    }

    std::vector<Mangled_parameter> params = mangled_parameters( ast, types, declaration );

    // The parameters are written in the declaration's own `T`, so without this an instance's symbol
    // would name a type parameter rather than the type it was instantiated at.
    const Bindings bindings = instance_bindings( ast, types, declaration, type_arguments );

    for( Mangled_parameter& param : params )
    {
        param.type = types.table().substitute( param.type, bindings );
    }

    if( ast.kind( declaration ) == Node_kind::Constructor_decl )
    {
        return mangle_constructor(
            "", interner.text( Symbol_id { ast.aux( declaration ) } ), type_arguments, params, types.table()
        );
    }

    if( ast.kind( declaration ) == Node_kind::Method_decl )
    {
        // The receiver, substituted above - so the tag names `Box<i32>` rather than `Box<T>` - and
        // then dropped from the parameters, since it can never be what tells two members apart. A
        // static method has no receiver to read either from, so the tag is built from the aggregate
        // and every written parameter stays: that is why the tag leads the argtypes rather than
        // sitting on parameter 0, and it is the whole of what M7 asked of this scheme.
        const bool    receiver = has_receiver( ast, declaration );
        const Node_id owner    = receiver ? Node_id {} : enclosing_aggregate( ast, declaration );
        const Type_id enclosing =
            receiver ? params.front().type
                     : types.table().structure( owner, type_arguments, interner.text( Symbol_id { ast.aux( owner ) } ) );

        return mangle_function(
            "",
            interner.text( Symbol_id { ast.aux( declaration ) } ),
            std::span( params ).subspan( receiver ? 1 : 0 ),
            types.table(),
            type_arguments,
            enclosing
        );
    }

    return mangle_function( "", interner.text( Symbol_id { ast.aux( declaration ) } ), params, types.table(), type_arguments );
}

std::string Spelling::destructor_of( Type_id type ) const
{
    const Node_id declaration = types.table().get( type ).declaration;

    for( const Node_id member : ast.members( declaration ) )
    {
        if( ast.kind( member ) == Node_kind::Destructor_decl )
        {
            return function( member, types.table().get( type ).arguments );
        }
    }

    assert( false && "a Drop names a type with no destructor of its own" );
    return {};
}

std::string Spelling::return_type( Node_id declaration ) const
{
    return type( binding_type( ast, types, declaration ) );
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
