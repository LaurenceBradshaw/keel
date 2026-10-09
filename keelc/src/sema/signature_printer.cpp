// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/signature_printer.h"
#include <fmt/format.h>
#include <algorithm>
#include <vector>
#include "lex/lexer.h"

namespace keel
{
namespace
{

class Signature_printer
{
public:
    Signature_printer(
        const Ast& ast, const Resolution& resolution, const Types& types, const Interner& interner, const Bindings& bindings
    )
        : ast_( ast ),
          resolution_( resolution ),
          types_( types ),
          interner_( interner ),
          bindings_( bindings )
    {
    }

    std::string declaration( Node_id decl ) const;

private:
    std::string type( Node_id annotation ) const;
    std::string recorded_type( Node_id decl ) const;
    std::string binding( Node_id decl ) const;
    std::string parameters( Node_id fn ) const;
    std::string type_parameters( Node_id list ) const;
    std::string where_clauses( Node_id list ) const;
    std::string type_parameter( Node_id param ) const;
    std::string access( Node_id member ) const;
    Type_id     bound( Node_id param ) const;

    std::string_view text( Symbol_id name ) const
    {
        return interner_.text( name );
    }

    const Ast&        ast_;
    const Resolution& resolution_;
    const Types&      types_;
    const Interner&   interner_;
    const Bindings&   bindings_;
};

std::string Signature_printer::declaration( Node_id decl ) const
{
    switch( ast_.kind( decl ) )
    {
    case Node_kind::Function_decl:
        return fmt::format(
            "{}{} {}{}{}{}",
            ast_.is_extern( decl ) ? "extern " : "",
            type( ast_.return_type( decl ) ),
            text( ast_.name( decl ) ),
            type_parameters( ast_.type_param_list( decl ) ),
            parameters( decl ),
            where_clauses( ast_.type_param_list( decl ) )
        );
    case Node_kind::Method_decl:
        return fmt::format(
            "{}{}{} {}{}{}",
            access( decl ),
            ast_.is_static_method( decl ) ? "static " : "",
            type( ast_.return_type( decl ) ),
            text( ast_.name( decl ) ),
            parameters( decl ),
            ast_.is_const_method( decl ) ? " const" : ""
        );
    case Node_kind::Constructor_decl:
        return fmt::format( "{}{}{}", access( decl ), text( ast_.name( decl ) ), parameters( decl ) );
    case Node_kind::Destructor_decl:
        return fmt::format( "~{}()", text( ast_.name( decl ) ) );
    case Node_kind::Struct_decl:
    case Node_kind::Class_decl:
        return fmt::format(
            "{} {}{}{}",
            ast_.kind( decl ) == Node_kind::Struct_decl ? "struct" : "class",
            text( ast_.name( decl ) ),
            type_parameters( ast_.type_param_list( decl ) ),
            where_clauses( ast_.type_param_list( decl ) )
        );
    case Node_kind::Enum_decl:
    {
        const Node_id underlying = ast_.underlying_type( decl );
        return fmt::format(
            "enum {}{}{}{}",
            text( ast_.name( decl ) ),
            type_parameters( ast_.type_param_list( decl ) ),
            underlying.is_valid() ? fmt::format( " : {}", type( underlying ) ) : "",
            where_clauses( ast_.type_param_list( decl ) )
        );
    }
    case Node_kind::Variant_decl:
    {
        std::string payload;
        for( const Node_id field : ast_.payload( decl ) )
        {
            payload += payload.empty() ? "( " : ", ";
            payload += binding( field );
        }
        return fmt::format( "{}{}", text( ast_.name( decl ) ), payload.empty() ? "" : payload + " )" );
    }
    case Node_kind::Field_decl:
        // A variant's payload field belongs to no aggregate and has no access to write.
        return ast_.enclosing_aggregate( decl ).is_valid() ? access( decl ) + binding( decl ) : binding( decl );
    case Node_kind::Var_decl:
        // Only a static field is a Var_decl among an aggregate's members.
        return ast_.enclosing_aggregate( decl ).is_valid() ? access( decl ) + "static " + binding( decl ) : binding( decl );
    case Node_kind::Param_decl:
    case Node_kind::Binding_decl:
        return binding( decl );
    case Node_kind::Type_param_decl:
        return type_parameter( decl );
    default:
        return "";
    }
}

// `const ref T value`: the written annotation, or the checker's type where `auto` or a pattern left
// none to read.
std::string Signature_printer::binding( Node_id decl ) const
{
    const Node_id     annotation = ast_.kind( decl ) == Node_kind::Binding_decl ? Node_id {} : ast_.annotation( decl );
    const std::string written    = annotation.is_valid() ? type( annotation ) : recorded_type( decl );
    return fmt::format( "{} {}", written, text( ast_.name( decl ) ) );
}

std::string Signature_printer::recorded_type( Node_id decl ) const
{
    const Type_id recorded = types_.type_of( decl );
    return recorded.is_valid() ? std::string( types_.table().name( recorded ) ) : "auto";
}

std::string Signature_printer::type( Node_id annotation ) const
{
    if( !annotation.is_valid() )
    {
        return "?";
    }

    switch( ast_.kind( annotation ) )
    {
    case Node_kind::Named_type:
    {
        const Node_id declared = resolution_.declaration_of( annotation );

        if( declared.is_valid() && ast_.kind( declared ) == Node_kind::Type_param_decl )
        {
            return type_parameter( declared );
        }

        const Node_id package = ast_.package( annotation );
        return package.is_valid() ? fmt::format( "{}::{}", text( ast_.name( package ) ), text( ast_.name( annotation ) ) )
                                  : std::string( text( ast_.name( annotation ) ) );
    }
    case Node_kind::Generic_type:
    {
        std::string arguments;
        for( const Node_id argument : ast_.type_args( annotation ) )
        {
            arguments += arguments.empty() ? "" : ", ";
            arguments += type( argument );
        }
        return fmt::format( "{}<{}>", type( ast_.generic_name( annotation ) ), arguments );
    }
    case Node_kind::Pointer_type:
        return type( ast_.inner_type( annotation ) ) + "*";
    case Node_kind::Many_pointer_type:
        return type( ast_.inner_type( annotation ) ) + "[*]";
    case Node_kind::Mode_type:
        return fmt::format(
            "{} {}", text( Interner::keyword( ast_.keyword( annotation ) ) ), type( ast_.inner_type( annotation ) )
        );
    case Node_kind::Const_type:
    {
        // A `const` after a pointer makes the pointer itself const: `u8* const`.
        const Node_id inner = ast_.inner_type( annotation );
        const bool    wraps_pointer =
            ast_.kind( inner ) == Node_kind::Pointer_type || ast_.kind( inner ) == Node_kind::Many_pointer_type;
        return wraps_pointer ? type( inner ) + " const" : "const " + type( inner );
    }
    case Node_kind::Function_type:
    {
        std::string params;
        for( const Node_id param : ast_.params( annotation ) )
        {
            params += params.empty() ? " " : ", ";
            params += type( ast_.annotation( param ) );
        }
        return fmt::format( "fn({}{}) -> {}", params, params.empty() ? "" : " ", type( ast_.return_type( annotation ) ) );
    }
    case Node_kind::Field_type:
        return fmt::format( "field( {} ) -> {}", type( ast_.child( annotation, 1 ) ), type( ast_.child( annotation, 0 ) ) );
    case Node_kind::Union_type:
    {
        std::string members;
        for( const Node_id member : ast_.children( annotation ) )
        {
            members += members.empty() ? "" : " | ";
            members += type( member );
        }
        return members;
    }
    default:
        return "?";
    }
}

// `( move T value, u64 n )`, or `()`; a receiver is not written.
std::string Signature_printer::parameters( Node_id fn ) const
{
    std::string params;
    for( const Node_id param : ast_.explicit_params( fn ) )
    {
        params += params.empty() ? "( " : ", ";
        params += binding( param );
    }
    return params.empty() ? "()" : params + " )";
}

std::string Signature_printer::type_parameters( Node_id list ) const
{
    std::string params;
    for( const Node_id param : ast_.type_parameters( list ) )
    {
        params += params.empty() ? "<" : ", ";
        params += type_parameter( param );
    }
    return params.empty() ? "" : params + ">";
}

// ` where T : Copyable & Numeric, where U : Copyable`, for the parameters still unbound.
std::string Signature_printer::where_clauses( Node_id list ) const
{
    if( !list.is_valid() )
    {
        return "";
    }

    const std::vector<Node_id> params = ast_.type_parameters( list );

    std::string clauses;
    for( const Node_id clause : ast_.where_clauses( list ) )
    {
        const auto subject =
            std::ranges::find_if( params, [&]( Node_id param ) { return ast_.name( param ) == ast_.name( clause ); } );

        if( subject != params.end() && bound( *subject ).is_valid() )
        {
            continue;
        }

        std::string bounds;
        for( const Node_id bound_name : ast_.bounds( clause ) )
        {
            bounds += bounds.empty() ? "" : " & ";
            bounds += text( ast_.name( bound_name ) );
        }

        clauses += fmt::format( "{} where {} : {}", clauses.empty() ? "" : ",", text( ast_.name( clause ) ), bounds );
    }
    return clauses;
}

std::string Signature_printer::type_parameter( Node_id param ) const
{
    const Type_id argument = bound( param );
    return argument.is_valid() ? std::string( types_.table().name( argument ) ) : std::string( text( ast_.name( param ) ) );
}

// Written for every member, `public` included, so a reader need not know each aggregate's default.
std::string Signature_printer::access( Node_id member ) const
{
    return ast_.access( member ) == Access::Private ? "private " : "public ";
}

Type_id Signature_printer::bound( Node_id param ) const
{
    const Type_id parameter = types_.type_of( param );
    const auto    found     = parameter.is_valid() ? bindings_.find( parameter.v ) : bindings_.end();
    return found == bindings_.end() ? Type_id {} : found->second;
}

} // namespace

std::string print_signature(
    const Ast&        ast,
    const Resolution& resolution,
    const Types&      types,
    const Interner&   interner,
    Node_id           decl,
    const Bindings&   bindings
)
{
    return Signature_printer( ast, resolution, types, interner, bindings ).declaration( decl );
}

std::string documentation( const Ast& ast, const Source_manager& sm, Node_id decl )
{
    const Span doc = ast.doc_span( decl );
    return doc.is_valid() ? doc_comment( sm.text( doc ), "///" ) : std::string {};
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <iostream>

#include "common/literal_pool.h"
#include "lex/lexer.h"
#include "parse/parser.h"

namespace keel
{
namespace
{

// The program `source`, checked, with each declaration found by its kind and name.
class Printed
{
public:
    explicit Printed( std::string_view source )
    {
        const File_id file = sm_.add_file( "t.kl", std::string( source ) );
        ast_               = parse( lex( file, sm_, interner_, literals_, diags_ ), sm_, diags_ );
        resolution_        = resolve( ast_, sm_, interner_, diags_ );
        types_             = type_check( ast_, resolution_, literals_, sm_, interner_, diags_ );
        diags_.render( sm_, std::cerr, false );
        REQUIRE_FALSE( diags_.has_errors() );
    }

    Node_id find( Node_kind kind, std::string_view name ) const
    {
        for( u32 i = 0; i < ast_.node_count(); ++i )
        {
            if( ast_.kind( Node_id { i } ) == kind && interner_.text( ast_.name( Node_id { i } ) ) == name )
            {
                return Node_id { i };
            }
        }
        FAIL( "no such declaration" );
        return Node_id {};
    }

    std::string operator()( Node_kind kind, std::string_view name, const Bindings& bindings = {} ) const
    {
        return print_signature( ast_, resolution_, types_, interner_, find( kind, name ), bindings );
    }

    // The bindings of the aggregate the parameter `variable` holds.
    Bindings bindings_of( std::string_view variable ) const
    {
        return aggregate_bindings(
            ast_, types_.table(), types_.type_of( find( Node_kind::Param_decl, variable ) ), types_.recorded()
        );
    }

private:
    Source_manager sm_;
    Interner       interner_;
    Literal_pool   literals_;
    Diagnostics    diags_;
    Ast            ast_;
    Resolution     resolution_;
    Types          types_;
};

TEST_CASE( "signatures_print_functions_as_written", "[sema][signature]" )
{
    const Printed printed( "i32 add( i32 a, const ref i32 b ) { return a + b; }\n"
                           "void none() {}\n"
                           "extern void kl_rt_write( i32 stream, const u8[*] data, u64 size );\n"
                           "T larger<T>( T a, T b ) where T : Copyable & Numeric { return a; }\n"
                           "void take( fn( i32 ) -> bool p, i32* const q, const i32* r ) {}\n" );

    REQUIRE( printed( Node_kind::Function_decl, "add" ) == "i32 add( i32 a, const ref i32 b )" );
    REQUIRE( printed( Node_kind::Function_decl, "none" ) == "void none()" );
    REQUIRE(
        printed( Node_kind::Function_decl, "kl_rt_write" ) ==
        "extern void kl_rt_write( i32 stream, const u8[*] data, u64 size )"
    );
    REQUIRE( printed( Node_kind::Function_decl, "larger" ) == "T larger<T>( T a, T b ) where T : Copyable & Numeric" );
    REQUIRE( printed( Node_kind::Function_decl, "take" ) == "void take( fn( i32 ) -> bool p, i32* const q, const i32* r )" );
    REQUIRE( printed( Node_kind::Param_decl, "b" ) == "const ref i32 b" );
    REQUIRE( printed( Node_kind::Type_param_decl, "T" ) == "T" );
}

TEST_CASE( "signatures_print_members_with_their_access", "[sema][signature]" )
{
    const Printed printed( "class Box<T>\n"
                           "{\n"
                           "    public Box( move T v ) { value = move v; count = 1; }\n"
                           "    ~Box() {}\n"
                           "    public void push( move T v ) { value = move v; }\n"
                           "    public const T* get() const { return &value; }\n"
                           "    public static Box<T> of( move T v ) { return Box<T>( move v ); }\n"
                           "    private T value;\n"
                           "    const i32 count;\n"
                           "    static i32 made = 0;\n"
                           "};\n" );

    REQUIRE( printed( Node_kind::Class_decl, "Box" ) == "class Box<T>" );
    REQUIRE( printed( Node_kind::Constructor_decl, "Box" ) == "public Box( move T v )" );
    REQUIRE( printed( Node_kind::Destructor_decl, "Box" ) == "~Box()" );
    REQUIRE( printed( Node_kind::Method_decl, "push" ) == "public void push( move T v )" );
    REQUIRE( printed( Node_kind::Method_decl, "get" ) == "public const T* get() const" );
    REQUIRE( printed( Node_kind::Method_decl, "of" ) == "public static Box<T> of( move T v )" );
    REQUIRE( printed( Node_kind::Field_decl, "value" ) == "private T value" );
    REQUIRE( printed( Node_kind::Field_decl, "count" ) == "private const i32 count" );
    REQUIRE( printed( Node_kind::Var_decl, "made" ) == "private static i32 made" );
}

TEST_CASE( "signatures_print_enums_and_variants", "[sema][signature]" )
{
    const Printed printed( "enum Colour : u8 { Red, Green };\n"
                           "enum Shape { Circle( f64 r ), Rect( f64 w, f64 h ) };\n" );

    REQUIRE( printed( Node_kind::Enum_decl, "Colour" ) == "enum Colour : u8" );
    REQUIRE( printed( Node_kind::Variant_decl, "Red" ) == "Red" );
    REQUIRE( printed( Node_kind::Variant_decl, "Rect" ) == "Rect( f64 w, f64 h )" );
    REQUIRE( printed( Node_kind::Field_decl, "w" ) == "f64 w" );
}

TEST_CASE( "signatures_print_inferred_locals_by_their_type", "[sema][signature]" )
{
    const Printed printed( "enum Shape { Circle( f64 r ), Dot };\n"
                           "f64 f( Shape s ) { auto k = 2.0; switch( s ) { case Shape::Circle( q ): return q * k; "
                           "default: return 0.0; } }\n" );

    REQUIRE( printed( Node_kind::Var_decl, "k" ) == "f64 k" );
    REQUIRE( printed( Node_kind::Binding_decl, "q" ) == "f64 q" );
}

// Every parameter the instance binds reads as its argument, and its `where` clause goes with it.
TEST_CASE( "signatures_print_a_member_at_an_instance", "[sema][signature]" )
{
    const Printed printed( "struct Pair<A, B> where A : Copyable, where B : Copyable\n"
                           "{\n"
                           "    A first;\n"
                           "    B* second;\n"
                           "    void set( move A a, Pair<B, A>* other ) {}\n"
                           "};\n"
                           "void f( Pair<i32, bool> p ) {}\n" );

    const Bindings at = printed.bindings_of( "p" );

    REQUIRE( printed( Node_kind::Struct_decl, "Pair" ) == "struct Pair<A, B> where A : Copyable, where B : Copyable" );
    REQUIRE( printed( Node_kind::Struct_decl, "Pair", at ) == "struct Pair<i32, bool>" );
    REQUIRE( printed( Node_kind::Field_decl, "second", at ) == "public bool* second" );
    REQUIRE( printed( Node_kind::Method_decl, "set", at ) == "public void set( move i32 a, Pair<bool, i32>* other )" );
}

} // namespace
} // namespace keel
#endif // ENABLE_UNIT_TESTS
