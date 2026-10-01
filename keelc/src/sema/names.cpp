// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/names.h"
#include <algorithm>
#include <optional>
#include <unordered_set>
#include "common/json.h"

namespace keel
{
namespace
{

class Name_collector
{
public:
    Name_collector( const Ast& ast, const Resolution& resolution, const Source_manager& sm, const Interner& interner )
        : ast_( ast ),
          resolution_( resolution ),
          sm_( sm ),
          interner_( interner )
    {
        for( const Node_id decl : ast_.children( ast_.root() ) )
        {
            top_level_.insert( decl.v );
        }
    }

    std::vector<Name> run();

private:
    std::optional<Name_kind> kind_of( Node_id id ) const;
    std::optional<Name_kind> declaration_kind( Node_id decl ) const;
    std::optional<Name_kind> member_kind( Node_id type, Symbol_id name ) const;
    Span                     name_span( Node_id id ) const;
    bool                     is_qualified( Node_id id ) const;

    const Ast&            ast_;
    const Resolution&     resolution_;
    const Source_manager& sm_;
    const Interner&       interner_;

    std::unordered_set<u32> top_level_;
};

std::vector<Name> Name_collector::run()
{
    std::vector<Name> names;

    if( !ast_.root().is_valid() )
    {
        return names;
    }

    for( u32 i = 0; i < ast_.node_count(); ++i )
    {
        const Node_id id { i };

        if( const std::optional<Name_kind> kind = kind_of( id ) )
        {
            // Synthesised nodes carry a borrowed span, which the name's text will not match.
            if( const Span span = name_span( id ); span.is_valid() )
            {
                names.push_back( { span, *kind } );
            }
        }
    }

    std::ranges::sort(
        names,
        []( const Name& a, const Name& b )
        { return std::pair( a.span.file.v, a.span.start ) < std::pair( b.span.file.v, b.span.start ); }
    );

    const auto duplicates = std::ranges::unique(
        names, []( const Name& a, const Name& b ) { return a.span.file == b.span.file && a.span.start == b.span.start; }
    );
    names.erase( duplicates.begin(), duplicates.end() );

    return names;
}

std::optional<Name_kind> Name_collector::kind_of( Node_id id ) const
{
    const Node_id decl = resolution_.declaration_of( id );

    switch( ast_.kind( id ) )
    {
    case Node_kind::Name_expr:
        if( Symbol_id { ast_.aux( id ) } == Interner::keyword( Keyword::This ) )
        {
            return std::nullopt;
        }
        if( decl.is_valid() )
        {
            return declaration_kind( decl );
        }
        if( ast_.children( id ).empty() && resolution_.is_package( Symbol_id { ast_.aux( id ) } ) )
        {
            return Name_kind::Package;
        }
        return std::nullopt;
    case Node_kind::Named_type:
    case Node_kind::Struct_literal:
        return decl.is_valid() ? declaration_kind( decl ) : std::nullopt;
    case Node_kind::Path_expr:
    {
        if( decl.is_valid() )
        {
            return declaration_kind( decl );
        }

        // `Colour::Red` and `Buffer::of`: the checker binds these, so the type is asked here.
        const Node_id type = resolution_.declaration_of( ast_.child( id, 0 ) );
        return type.is_valid() ? member_kind( type, Symbol_id { ast_.aux( id ) } ) : std::nullopt;
    }
    case Node_kind::Type_param_decl:
    case Node_kind::Binding_decl:
        return declaration_kind( id );
    default:
        return std::nullopt;
    }
}

std::optional<Name_kind> Name_collector::declaration_kind( Node_id decl ) const
{
    switch( ast_.kind( decl ) )
    {
    case Node_kind::Var_decl:
        return top_level_.contains( decl.v ) ? Name_kind::Global : Name_kind::Variable;
    case Node_kind::Binding_decl:
        return Name_kind::Variable;
    case Node_kind::Param_decl:
        return Name_kind::Parameter;
    case Node_kind::Field_decl:
        return Name_kind::Field;
    case Node_kind::Function_decl:
        return Name_kind::Function;
    case Node_kind::Method_decl:
        return Name_kind::Method;
    case Node_kind::Struct_decl:
        return Name_kind::Struct;
    case Node_kind::Class_decl:
        return Name_kind::Class;
    case Node_kind::Enum_decl:
        return Name_kind::Enum;
    case Node_kind::Variant_decl:
        return Name_kind::Variant;
    case Node_kind::Type_param_decl:
        return Name_kind::Type_parameter;
    default:
        return std::nullopt;
    }
}

std::optional<Name_kind> Name_collector::member_kind( Node_id type, Symbol_id name ) const
{
    if( ast_.kind( type ) == Node_kind::Enum_decl )
    {
        for( const Node_id variant : ast_.variants( type ) )
        {
            if( Symbol_id { ast_.aux( variant ) } == name )
            {
                return Name_kind::Variant;
            }
        }
    }
    else if( is_aggregate( ast_.kind( type ) ) )
    {
        for( const Node_id member : ast_.members( type ) )
        {
            if( Symbol_id { ast_.aux( member ) } == name &&
                ( ast_.kind( member ) == Node_kind::Method_decl || ast_.kind( member ) == Node_kind::Field_decl ) )
            {
                return declaration_kind( member );
            }
        }
    }

    return std::nullopt;
}

bool Name_collector::is_qualified( Node_id id ) const
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Path_expr:
        return true;
    case Node_kind::Named_type:
        return !ast_.children( id ).empty();
    case Node_kind::Struct_literal:
        return ast_.initialisers( id ).size() != ast_.children( id ).size();
    default:
        return false;
    }
}

// The parser ends a qualified name's span on the name, and starts every other one with it; a
// qualified struct literal's span runs on to its `}`, so its name is found after the `::`.
Span Name_collector::name_span( Node_id id ) const
{
    const std::string_view name = interner_.text( Symbol_id { ast_.aux( id ) } );
    const Span             node = ast_.span( id );
    const std::string_view text = sm_.file( node.file ).text;

    u32 start = node.start;

    if( ast_.kind( id ) == Node_kind::Struct_literal && is_qualified( id ) )
    {
        start = ast_.span( ast_.child( id, 0 ) ).end;
        while( start < text.size() && ( text[start] == ':' || text[start] == ' ' || text[start] == '\t' ) )
        {
            ++start;
        }
    }
    else if( is_qualified( id ) )
    {
        start = node.end >= name.size() ? node.end - static_cast<u32>( name.size() ) : node.end;
    }

    const Span span { node.file, start, start + static_cast<u32>( name.size() ) };

    if( name.empty() || span.end > text.size() || text.substr( span.start, name.size() ) != name )
    {
        return Span {};
    }

    return span;
}

} // namespace

std::string_view name_kind_name( Name_kind kind )
{
    switch( kind )
    {
    case Name_kind::Global:
        return "global";
    case Name_kind::Variable:
        return "variable";
    case Name_kind::Parameter:
        return "parameter";
    case Name_kind::Field:
        return "field";
    case Name_kind::Function:
        return "function";
    case Name_kind::Method:
        return "method";
    case Name_kind::Struct:
        return "struct";
    case Name_kind::Class:
        return "class";
    case Name_kind::Enum:
        return "enum";
    case Name_kind::Variant:
        return "variant";
    case Name_kind::Type_parameter:
        return "type_parameter";
    case Name_kind::Package:
        return "package";
    }

    return "?";
}

std::vector<Name>
collect_names( const Ast& ast, const Resolution& resolution, const Source_manager& sm, const Interner& interner )
{
    return Name_collector( ast, resolution, sm, interner ).run();
}

void render_names_json( const Source_manager& sm, const std::vector<Name>& names, std::ostream& out )
{
    for( const Name& name : names )
    {
        const Line_col start = sm.line_col( name.span.file, name.span.start );
        const Line_col end   = sm.line_col( name.span.file, name.span.end );

        out << R"({"kind":"name","refers_to":")" << name_kind_name( name.kind ) << R"(","file":")"
            << json_escape( sm.file( name.span.file ).path ) << R"(","line":)" << start.line << R"(,"col":)" << start.col
            << R"(,"end_col":)" << end.col << "}\n";
    }
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include "common/literal_pool.h"
#include "lex/lexer.h"
#include "parse/parser.h"

namespace keel
{
namespace
{

// Each name as "text:kind", in source order.
std::vector<std::string> named( std::string_view source )
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;

    const File_id    file       = sm.add_file( "t.kl", std::string( source ) );
    const Ast        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
    const Resolution resolution = resolve( ast, sm, interner, diags );

    std::vector<std::string> out;
    for( const Name& name : collect_names( ast, resolution, sm, interner ) )
    {
        out.push_back( fmt::format( "{}:{}", sm.text( name.span ), name_kind_name( name.kind ) ) );
    }
    return out;
}

bool has( const std::vector<std::string>& names, std::string_view entry )
{
    return std::ranges::find( names, entry ) != names.end();
}

TEST_CASE( "names_a_lowercase_class_called_as_a_constructor", "[sema][names]" )
{
    const auto names = named( "class meter { i32 n; public meter( i32 s ) { n = s; } };\n"
                              "void f() { meter m = meter( 1 ); }" );

    // The constructor's own name, the annotation and the call.
    REQUIRE( std::ranges::count( names, std::string( "meter:class" ) ) == 3 );
    REQUIRE( has( names, "n:field" ) );
    REQUIRE( has( names, "s:parameter" ) );
}

TEST_CASE( "names_variants_through_their_enum", "[sema][names]" )
{
    const auto names = named( "enum Colour { Red, Green };\n"
                              "i32 f( Colour c ) { if( c == Colour::Green ) { return 1; } return 0; }" );

    REQUIRE( has( names, "Colour:enum" ) );
    REQUIRE( has( names, "Green:variant" ) );
    REQUIRE( has( names, "c:parameter" ) );
}

TEST_CASE( "names_pattern_bindings_and_payload_variants", "[sema][names]" )
{
    const auto names = named( "enum Shape { Circle( f64 r ), Dot };\n"
                              "f64 f( Shape s ) { switch( s ) { case Shape::Circle( r ): return r; default: return 0.0; } }" );

    REQUIRE( has( names, "Circle:variant" ) );
    REQUIRE( std::ranges::count( names, std::string( "r:variable" ) ) == 2 );
}

TEST_CASE( "names_type_parameters_where_declared_and_used", "[sema][names]" )
{
    const auto names = named( "T larger<T>( T a, T b ) { return a; }" );

    REQUIRE( std::ranges::count( names, std::string( "T:type_parameter" ) ) == 4 );
    REQUIRE( has( names, "a:parameter" ) );
}

TEST_CASE( "names_globals_apart_from_locals", "[sema][names]" )
{
    const auto names = named( "i32 counter = 0;\nvoid f() { i32 n = counter; n = n + 1; }" );

    REQUIRE( has( names, "counter:global" ) );
    REQUIRE( has( names, "n:variable" ) );
    REQUIRE_FALSE( has( names, "counter:variable" ) );
}

TEST_CASE( "names_static_methods_through_their_class", "[sema][names]" )
{
    const auto names =
        named( "class Box { i32 v; public Box( i32 x ) { v = x; } static Box of( i32 x ) { return Box( x ); } };\n"
               "void f() { Box b = Box::of( 1 ); }" );

    REQUIRE( has( names, "of:method" ) );
}

TEST_CASE( "names_struct_literals_by_their_type", "[sema][names]" )
{
    const auto names = named( "struct Point { i32 x; i32 y; };\nvoid f() { Point p = Point { 1, 2 }; }" );

    REQUIRE( std::ranges::count( names, std::string( "Point:struct" ) ) == 2 );
}

TEST_CASE( "names_skip_this_and_builtins", "[sema][names]" )
{
    const auto names = named( "struct P { i32 x; i32 get() const { return this.x; } };" );

    for( const std::string& name : names )
    {
        REQUIRE( name.rfind( "this:", 0 ) != 0 );
        REQUIRE( name.rfind( "i32:", 0 ) != 0 );
    }
}

TEST_CASE( "names_render_as_json_lines", "[sema][names]" )
{
    Source_manager sm;
    const File_id  file = sm.add_file( "a.kl", "i32 x;\nT y;" );

    std::ostringstream out;
    render_names_json( sm, { { Span { file, 9, 10 }, Name_kind::Global } }, out );

    REQUIRE(
        out.str() == "{\"kind\":\"name\",\"refers_to\":\"global\",\"file\":\"a.kl\",\"line\":2,\"col\":3,\"end_col\":4}\n"
    );
}

} // namespace
} // namespace keel
#endif // ENABLE_UNIT_TESTS
