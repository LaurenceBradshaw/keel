// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

    for( const Node_id param : ast.params( decl ) )
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

    for( const Node_id param : ast.params( decl ) )
    {
        const Keyword mode = ast.call_marker( param );

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
    const std::vector<Node_id> parameters = ast.type_parameters( ast.type_param_list( declaration ) );

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
    case Type_kind::Never:
        return "void";

    case Type_kind::Bool:
        return "bool"; // <stdbool.h>'s, per §7.8

    case Type_kind::Int:
        // §7.8: exact-width types, never C's own, whose sizes are a platform question.
        return fmt::format( "{}int{}_t", described.is_signed ? "" : "u", described.width );

    case Type_kind::Float:
        return described.width == 32 ? "float" : "double";

    case Type_kind::Struct:
    case Type_kind::Union:
        return structure( type );

    case Type_kind::Pointer:
        if( types.table().is_interface( described.element ) )
        {
            return interface_reference( described.element );
        }
        if( types.table().is_run( described.element ) )
        {
            return run_reference( described.element );
        }

        return fmt::format( "{}*", this->type( described.element ) );

    case Type_kind::Many_pointer:
        return fmt::format( "{}*", this->type( described.element ) );

    // The typedef's name, written out by the emitter before anything can be declared with it.
    case Type_kind::Function:
        return mangle_function_type( "", type, types.table() );

    // An offset in bytes; which aggregate and which field is the checker's business, not C's.
    case Type_kind::Field:
        return "size_t";

    // A payload-free enum is its underlying integer - not a C `enum`, whose type is
    // implementation-defined and which would buy nothing, since by here Keel has already erased the
    // distinction the tag existed for. One carrying payloads is a struct: a tag and the fields.
    case Type_kind::Enum:
        return ast.enum_has_payload( described.declaration ) ? structure( type ) : this->type( described.element );

    default:
        assert( false && "no C spelling for this type" );
        return "void";
    }
}

std::string Spelling::structure( Type_id type ) const
{
    if( types.table().is_union( type ) )
    {
        return fmt::format( "struct {}", mangle_union( type, types.table() ) );
    }

    return fmt::format(
        "struct {}", mangle_struct( package_name( types.table().get( type ).declaration ), type, types.table() )
    );
}

std::string Spelling::interface_reference( Type_id interface ) const
{
    return fmt::format(
        "struct {}__ref", mangle_struct( package_name( types.table().get( interface ).declaration ), interface, types.table() )
    );
}

std::string Spelling::run_reference( Type_id run ) const
{
    return fmt::format( "struct {}__ref", mangle_run( run, types.table() ) );
}

std::string Spelling::parameter( Type_id type, std::string_view name ) const
{
    if( types.table().is_run_pointer( type ) )
    {
        const auto element = types.table().get( types.table().get( type ).element ).element;
        if( !name.empty() )
        {
            return fmt::format( "{}* {}_data, uint64_t {}_size", this->type( element ), name, name );
        }
        return fmt::format( "{}*, uint64_t", this->type( element ) );
    }

    if( !name.empty() )
    {
        return fmt::format( "{} {}", this->type( type ), name );
    }

    return fmt::format( "{}", this->type( type ) );
}

std::string Spelling::argument( Type_id type, std::string_view name ) const
{
    if( types.table().is_run_pointer( type ) )
    {
        if( !name.empty() )
        {
            return fmt::format( "( {} ).data, ( {} ).size", name, name );
        }
    }

    if( !name.empty() )
    {
        return std::string( name );
    }

    return "";
}

std::string Spelling::vtable_type( Type_id interface ) const
{
    return fmt::format(
        "struct {}__vtable",
        mangle_struct( package_name( types.table().get( interface ).declaration ), interface, types.table() )
    );
}

std::string Spelling::vtable( Type_id implementer, Type_id interface ) const
{
    return fmt::format(
        "{}__as_{}",
        mangle_struct( package_name( types.table().get( implementer ).declaration ), implementer, types.table() ),
        mangle_struct( package_name( types.table().get( interface ).declaration ), interface, types.table() )
    );
}

std::string Spelling::thunk( Type_id implementer, Type_id interface, Node_id requirement ) const
{
    return fmt::format( "{}__{}", vtable( implementer, interface ), field( requirement ) );
}

std::string Spelling::drop_function( Type_id implementer ) const
{
    return fmt::format(
        "{}__drop", mangle_struct( package_name( types.table().get( implementer ).declaration ), implementer, types.table() )
    );
}

std::string Spelling::field( Node_id declaration ) const
{
    return mangle_local( interner.text( ast.name( declaration ) ), declaration.v );
}

std::string Spelling::function( Node_id declaration, std::span<const Type_id> type_arguments ) const
{
    // Declared by nothing, and named for its one type argument: a union's destructor, or an
    // implementer's drop function.
    if( !declaration.is_valid() )
    {
        return types.table().is_union( type_arguments[0] ) ? destructor_of( type_arguments[0] )
                                                           : drop_function( type_arguments[0] );
    }

    // An extern names a symbol someone else defined, so the Keel name is the C name: `kl__malloc__u64`
    // would not link against anything.
    if( ast.is_extern( declaration ) )
    {
        return std::string( interner.text( ast.name( declaration ) ) );
    }

    // A destructor's aux is the type's own name, and an instantiation is told apart by its type
    // arguments alone - the same rule mangle_function follows. An enum names its synthesised one.
    if( ast.kind( declaration ) == Node_kind::Destructor_decl || ast.kind( declaration ) == Node_kind::Enum_decl )
    {
        return mangle_destructor(
            package_name( declaration ), interner.text( ast.name( declaration ) ), type_arguments, types.table()
        );
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
            package_name( declaration ), interner.text( ast.name( declaration ) ), type_arguments, params, types.table()
        );
    }

    if( ast.kind( declaration ) == Node_kind::Method_decl )
    {
        // The receiver, substituted above - so the tag names `Box<i32>` rather than `Box<T>` - and
        // then dropped from the parameters, since it can never be what tells two members apart. A
        // static method has no receiver to read either from, so the tag is built from the aggregate
        // and every written parameter stays: that is why the tag leads the argtypes rather than
        // sitting on parameter 0, and it is the whole of what M7 asked of this scheme. The tag takes
        // only the aggregate's arguments; a generic method's own follow them.
        const bool        receiver      = ast.has_receiver( declaration );
        const Node_id     owner         = receiver ? Node_id {} : ast.enclosing_aggregate( declaration );
        const std::size_t num_type_args = receiver ? 0 : ast.type_parameters( ast.type_param_list( owner ) ).size();
        // A block's static method is tagged with the type the block is for.
        const Type_id enclosing =
            receiver ? params.front().type
            : ast.kind( owner ) == Node_kind::Implements_decl
                ? types.type_of( owner )
                : types.table().structure(
                      owner, std::span( type_arguments ).first( num_type_args ), interner.text( ast.name( owner ) )
                  );

        const Node_id block = ast.enclosing_aggregate( declaration );
        const Type_id conformed =
            ast.kind( block ) == Node_kind::Implements_decl ? types.type_of( ast.implemented( block )[0] ) : Type_id {};

        return mangle_function(
            package_name( declaration ),
            interner.text( ast.name( declaration ) ),
            std::span( params ).subspan( receiver ? 1 : 0 ),
            types.table(),
            type_arguments,
            enclosing,
            conformed
        );
    }

    return mangle_function(
        package_name( declaration ), interner.text( ast.name( declaration ) ), params, types.table(), type_arguments
    );
}

std::string Spelling::destructor_of( Type_id type ) const
{
    if( types.table().is_union( type ) )
    {
        return fmt::format( "{}__dtor", mangle_union( type, types.table() ) );
    }

    const Node_id declaration = types.table().get( type ).declaration;

    if( types.table().is_enum( type ) )
    {
        return function( declaration, types.table().get( type ).arguments );
    }

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

        rendered += parameter( params[i], "" );
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

std::string c_string( std::string_view str )
{
    std::string text = "\"";
    for( const char c : str )
    {
        if( c == '"' || c == '\\' || c == '?' || static_cast<unsigned char>( c ) < 0x20 ||
            static_cast<unsigned char>( c ) >= 0x7f )
        {
            text += fmt::format( "\\{:03o}", static_cast<unsigned char>( c ) );
        }
        else
        {
            text += c;
        }
    }
    text += "\"";
    return text;
}

std::string Spelling::global_definition( Node_id declaration, Type_id owner ) const
{
    const Type_id            variable  = types.type_of( declaration );
    std::string_view         name_text = interner.text( ast.name( declaration ) );
    std::span<const Type_id> arguments = owner.is_valid() ? types.table().get( owner ).arguments : std::span<const Type_id>();
    const std::string        name      = mangle_static_field( name_text, declaration.v, arguments, types.table() );

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
    else if( table.is_pointer( variable ) || table.is_many_pointer( variable ) )
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

std::string Spelling::package_name( Node_id decl ) const
{
    // A built-in type belongs to no package's file.
    if( !decl.is_valid() )
    {
        return "";
    }

    Symbol_id package_id = imports.package_of( ast.span( decl ).file );

    if( !package_id.is_valid() )
    {
        return "";
    }

    return std::string( interner.text( package_id ) );
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

// Three octal digits always: `\x` and a shorter octal escape would both swallow a digit after them.
TEST_CASE( "c_string_escapes_every_byte_c_could_misread", "[codegen][spelling]" )
{
    REQUIRE( c_string( "" ) == "\"\"" );
    REQUIRE( c_string( "hello" ) == "\"hello\"" );
    REQUIRE(
        c_string( std::string_view(
            "a\0"
            "1",
            3
        ) ) == "\"a\\0001\""
    );
    REQUIRE( c_string( "\"\\\n" ) == "\"\\042\\134\\012\"" );
    REQUIRE(
        c_string( "?"
                  "?=" ) == "\"\\077\\077=\""
    ); // a trigraph, which C11 still reads
    REQUIRE( c_string( "\xff" ) == "\"\\377\"" );
}

// A borrowed run crosses a C call as two parameters, so no ABI passes a struct by hidden pointer.
TEST_CASE( "a_run_parameter_is_spelled_as_two", "[codegen][spelling]" )
{
    const Ast      ast;
    Types          types;
    const Interner interner;
    const Imports  imports;
    const Spelling spelling { ast, types, interner, imports };

    Type_table&   table = types.table();
    const Type_id i32   = table.integer( 32, true );
    const Type_id u8    = table.integer( 8, false );
    const Type_id run   = table.pointer_to( table.run_of( i32 ) );

    SECTION( "a run pointer is its data and its size" )
    {
        REQUIRE( spelling.parameter( run, "kl_values_1" ) == "int32_t* kl_values_1_data, uint64_t kl_values_1_size" );
        REQUIRE( spelling.parameter( run, "" ) == "int32_t*, uint64_t" );
    }

    // The struct's `data` is not const either: Keel, not C, keeps a `const ref` read-only.
    SECTION( "a read-only run is spelled as the struct's field is" )
    {
        const Type_id text = table.pointer_to( table.run_of( u8 ), true );

        REQUIRE( spelling.parameter( text, "kl_text_2" ) == "uint8_t* kl_text_2_data, uint64_t kl_text_2_size" );
    }

    SECTION( "anything else is one parameter" )
    {
        REQUIRE( spelling.parameter( i32, "kl_n_3" ) == "int32_t kl_n_3" );
        REQUIRE( spelling.parameter( i32, "" ) == "int32_t" );
        REQUIRE( spelling.parameter( table.pointer_to( i32 ), "kl_p_4" ) == "int32_t* kl_p_4" );
        REQUIRE( spelling.parameter( table.many_pointer_to( i32 ), "" ) == "int32_t*" );
    }

    SECTION( "a run argument passes both halves" )
    {
        REQUIRE( spelling.argument( run, "kl_t5" ) == "( kl_t5 ).data, ( kl_t5 ).size" );
        REQUIRE( spelling.argument( i32, "kl_t6" ) == "kl_t6" );
        REQUIRE( spelling.argument( table.pointer_to( i32 ), "&kl_x_7" ) == "&kl_x_7" );
    }
}

} // namespace keel
#endif
