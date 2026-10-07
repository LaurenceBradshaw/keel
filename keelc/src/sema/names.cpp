// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/names.h"
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "common/json.h"

namespace keel
{
namespace
{

class Name_collector
{
public:
    Name_collector(
        const Ast& ast, const Resolution& resolution, const Types& types, const Source_manager& sm, const Interner& interner
    )
        : ast_( ast ),
          resolution_( resolution ),
          types_( types ),
          sm_( sm ),
          interner_( interner )
    {
        for( const Node_id decl : ast_.declarations( ast_.root() ) )
        {
            if( is_aggregate( ast_.kind( decl ) ) )
            {
                for( const Node_id member : ast_.members( decl ) )
                {
                    if( ast_.kind( member ) == Node_kind::Var_decl )
                    {
                        top_level_.insert( member.v );
                    }
                }
            }

            top_level_.insert( decl.v );
        }
    }

    std::vector<Name> run();

private:
    bool                     is_package( Node_id id ) const;
    Node_id                  referent( Node_id id ) const;
    std::optional<Name_kind> declaration_kind( Node_id decl ) const;
    Node_id                  member_named( Node_id type, Symbol_id name ) const;
    Node_id                  member_after_dot( Node_id id ) const;
    Span                     name_span( Node_id id ) const;
    Span                     declared_name_span( Node_id decl ) const;
    bool                     is_qualified( Node_id id ) const;

    const Ast&            ast_;
    const Resolution&     resolution_;
    const Types&          types_;
    const Source_manager& sm_;
    const Interner&       interner_;

    std::unordered_set<u32>          top_level_;
    std::unordered_map<u32, Node_id> called_; // a call's `.callee`, to the overload the checker chose
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

        if( ast_.kind( id ) == Node_kind::Call_expr && ast_.kind( ast_.callee( id ) ) == Node_kind::Field_expr )
        {
            called_.emplace( ast_.callee( id ).v, types_.callee_of( id ) );
        }
    }

    for( u32 i = 0; i < ast_.node_count(); ++i )
    {
        const Node_id id { i };

        const Node_id                  decl = referent( id );
        const std::optional<Name_kind> kind = is_package( id ) ? Name_kind::Package : declaration_kind( decl );

        // Synthesised nodes carry a borrowed span, which the name's text will not match.
        if( const Span span = kind ? name_span( id ) : Span {}; span.is_valid() )
        {
            names.push_back( { span, *kind, decl.is_valid() ? declared_name_span( decl ) : Span {} } );
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

bool Name_collector::is_package( Node_id id ) const
{
    return ast_.kind( id ) == Node_kind::Name_expr && !resolution_.declaration_of( id ).is_valid() &&
           !ast_.type_arg_list( id ).is_valid() && resolution_.is_package( ast_.name( id ) );
}

// The declaration a written name refers to, or invalid when it is not a name or resolved to nothing.
Node_id Name_collector::referent( Node_id id ) const
{
    const Node_id decl = resolution_.declaration_of( id );

    switch( ast_.kind( id ) )
    {
    case Node_kind::Name_expr:
        return ast_.name( id ) == Interner::keyword( Keyword::This ) ? Node_id {} : decl;
    case Node_kind::Named_type:
    case Node_kind::Struct_literal:
        return decl;
    case Node_kind::Path_expr:
    {
        if( decl.is_valid() )
        {
            return decl;
        }

        // `Colour::Red` and `Buffer::of`: the checker binds these, so the type is asked here.
        const Node_id type = resolution_.declaration_of( ast_.qualifier( id ) );
        return type.is_valid() ? member_named( type, ast_.name( id ) ) : Node_id {};
    }
    case Node_kind::Field_expr:
        return member_after_dot( id );
    case Node_kind::Type_param_decl:
    case Node_kind::Binding_decl:
        return id;
    default:
        return Node_id {};
    }
}

// `p.x` or `p.area()`, through a pointer as `.` reaches: the object's type says whose member it is.
Node_id Name_collector::member_after_dot( Node_id id ) const
{
    if( const auto call = called_.find( id.v ); call != called_.end() && call->second.is_valid() )
    {
        return call->second;
    }

    Type_id type = types_.type_of( ast_.object( id ) );

    if( !type.is_valid() )
    {
        return Node_id {};
    }

    if( types_.table().is_pointer( type ) )
    {
        type = types_.table().get( type ).element;
    }

    const Node_id aggregate = types_.table().get( type ).declaration;
    return aggregate.is_valid() ? member_named( aggregate, ast_.name( id ) ) : Node_id {};
}

std::optional<Name_kind> Name_collector::declaration_kind( Node_id decl ) const
{
    if( !decl.is_valid() )
    {
        return std::nullopt;
    }

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

Node_id Name_collector::member_named( Node_id type, Symbol_id name ) const
{
    if( ast_.kind( type ) == Node_kind::Enum_decl )
    {
        for( const Node_id variant : ast_.variants( type ) )
        {
            if( ast_.name( variant ) == name )
            {
                return variant;
            }
        }
    }
    else if( is_aggregate( ast_.kind( type ) ) )
    {
        for( const Node_id member : ast_.members( type ) )
        {
            if( ast_.name( member ) == name &&
                ( ast_.kind( member ) == Node_kind::Method_decl || ast_.kind( member ) == Node_kind::Field_decl ) )
            {
                return member;
            }
        }
    }

    return Node_id {};
}

bool Name_collector::is_qualified( Node_id id ) const
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Path_expr:
    case Node_kind::Field_expr:
        return true;
    case Node_kind::Named_type:
    case Node_kind::Struct_literal:
        return ast_.package( id ).is_valid();
    default:
        return false;
    }
}

// The parser ends a qualified name's span on the name, and starts every other one with it.
Span Name_collector::name_span( Node_id id ) const
{
    if( ast_.kind( id ) == Node_kind::Struct_literal )
    {
        return ast_.type_name_span( id );
    }

    const std::string_view name = interner_.text( ast_.name( id ) );
    const Span             node = ast_.span( id );
    const std::string_view text = sm_.file( node.file ).text;

    u32 start = node.start;

    if( is_qualified( id ) )
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

Span Name_collector::declared_name_span( Node_id decl ) const
{
    return ast_.name_span( decl );
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

std::vector<Name> collect_names(
    const Ast& ast, const Resolution& resolution, const Types& types, const Source_manager& sm, const Interner& interner
)
{
    return Name_collector( ast, resolution, types, sm, interner ).run();
}

void render_names_json( const Source_manager& sm, const std::vector<Name>& names, std::ostream& out )
{
    for( const Name& name : names )
    {
        const Line_col start = sm.line_col( name.span.file, name.span.start );
        const Line_col end   = sm.line_col( name.span.file, name.span.end );

        out << R"({"kind":"name","refers_to":")" << name_kind_name( name.kind ) << R"(","file":")"
            << json_escape( sm.file( name.span.file ).path ) << R"(","line":)" << start.line << R"(,"col":)" << start.col
            << R"(,"end_col":)" << end.col;

        // Where go-to-definition lands; absent for a package, which has no one declaration.
        if( name.declaration.is_valid() )
        {
            const Line_col decl_start = sm.line_col( name.declaration.file, name.declaration.start );
            const Line_col decl_end   = sm.line_col( name.declaration.file, name.declaration.end );

            out << R"(,"decl_file":")" << json_escape( sm.file( name.declaration.file ).path ) << R"(","decl_line":)"
                << decl_start.line << R"(,"decl_col":)" << decl_start.col << R"(,"decl_end_col":)" << decl_end.col;
        }

        out << "}\n";
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

// Each name as "text:kind", or "text@offset->offset" with `declarations`, in source order.
std::vector<std::string> named( std::string_view source, bool declarations = false )
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;

    const File_id    file       = sm.add_file( "t.kl", std::string( source ) );
    const Ast        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
    const Resolution resolution = resolve( ast, sm, interner, diags );
    const Types      types      = type_check( ast, resolution, literals, sm, interner, diags );

    std::vector<std::string> out;
    for( const Name& name : collect_names( ast, resolution, types, sm, interner ) )
    {
        out.push_back(
            declarations ? fmt::format( "{}@{}->{}", sm.text( name.span ), name.span.start, name.declaration.start )
                         : fmt::format( "{}:{}", sm.text( name.span ), name_kind_name( name.kind ) )
        );
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

// Bound by the checker, which alone knows the object's type: resolution never sees them.
TEST_CASE( "names_fields_and_methods_after_a_dot", "[sema][names]" )
{
    constexpr std::string_view source = "struct P\n"
                                        "{\n"
                                        "    i32 x;\n"
                                        "    i32 get() const { return x; }\n"
                                        "    i32 add( i32 a ) const { return x + a; }\n"
                                        "    i32 add( i32 a, i32 b ) const { return x + a + b; }\n"
                                        "};\n"
                                        "i32 f( P* q ) { P p = P { 1 }; return p.x + q.x + p.get() + p.add( 1, 2 ); }\n";

    SECTION( "as fields and methods" )
    {
        const auto names = named( source );

        REQUIRE( std::ranges::count( names, std::string( "x:field" ) ) == 5 ); // three bare in the methods, p.x and q.x
        REQUIRE( has( names, "get:method" ) );
        REQUIRE( has( names, "add:method" ) );
    }

    // `q.x` reaches through the pointer, and `add( 1, 2 )` lands on the overload the checker chose.
    SECTION( "pointing at their declarations" )
    {
        const auto   names = named( source, true );
        const auto   at    = [&]( std::string_view text, std::size_t nth = 0 ) { return source.find( text ) + nth; };
        const u32    x     = static_cast<u32>( at( "x;" ) );
        const u32    add2  = static_cast<u32>( source.find( "add", at( "i32 add( i32 a, i32 b )" ) ) );
        const size_t p_x   = source.find( "p.x" ) + 2;
        const size_t q_x   = source.find( "q.x" ) + 2;
        const size_t call  = source.find( "add( 1, 2 )" );

        REQUIRE( has( names, fmt::format( "x@{}->{}", p_x, x ) ) );
        REQUIRE( has( names, fmt::format( "x@{}->{}", q_x, x ) ) );
        REQUIRE( has( names, fmt::format( "add@{}->{}", call, add2 ) ) );
    }
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
    render_names_json(
        sm,
        { { Span { file, 9, 10 }, Name_kind::Global, Span { file, 4, 5 } }, { Span { file, 7, 8 }, Name_kind::Package, Span {} }
        },
        out
    );

    REQUIRE(
        out.str() == "{\"kind\":\"name\",\"refers_to\":\"global\",\"file\":\"a.kl\",\"line\":2,\"col\":3,\"end_col\":4,"
                     "\"decl_file\":\"a.kl\",\"decl_line\":1,\"decl_col\":5,\"decl_end_col\":6}\n"
                     "{\"kind\":\"name\",\"refers_to\":\"package\",\"file\":\"a.kl\",\"line\":2,\"col\":1,\"end_col\":2}\n"
    );
}

TEST_CASE( "names_point_at_the_declared_name", "[sema][names]" )
{
    const auto names =
        named( "enum Colour { Red }; class meter { i32 n; }; i32 f( Colour c ) { Colour d = c; return 0; }", true );

    REQUIRE( has( names, "Colour@52->5" ) );
    REQUIRE( has( names, "c@76->59" ) );
}

TEST_CASE( "names_point_past_a_type_spelt_like_a_prefix", "[sema][names]" )
{
    const auto names = named( "struct p { i32 v; };\np pp( p v ) { return v; }\nvoid g() { p x = pp( p { 1 } ); }", true );

    // `pp` is declared at 23, after its return type `p`.
    REQUIRE(
        std::ranges::any_of( names, []( const std::string& n ) { return n.starts_with( "pp@" ) && n.ends_with( "->23" ); } )
    );
}

// The parser's name token, not the first word of the declaration that matches it.
TEST_CASE( "names_point_past_a_type_spelt_like_the_name", "[sema][names]" )
{
    const auto names = named( "struct Point { i32 x; };\ni32 main() { Point Point = Point { 1 }; return Point.x; }", true );

    // The variable `Point` is declared at 44, after its type `Point` at 38.
    REQUIRE( has( names, "Point@72->44" ) );
}

// Storage for the whole program, as a file-scope variable is, wherever it is declared.
TEST_CASE( "names_static_fields_as_globals", "[sema][names][static]" )
{
    const auto names = named( "struct S { i32 x; static i32 made = 0; i32 get() const { return made; } };\n"
                              "i32 f() { return S::made; }" );

    REQUIRE( has( names, "made:global" ) );
    REQUIRE_FALSE( has( names, "made:variable" ) );
    REQUIRE_FALSE( has( names, "made:field" ) );
}

} // namespace
} // namespace keel
#endif // ENABLE_UNIT_TESTS
