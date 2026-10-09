// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/names.h"
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "common/json.h"
#include "sema/signature_printer.h"

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
    bool                     declares_here( Node_id id ) const;
    Node_id                  referent( Node_id id ) const;
    std::optional<Name_kind> declaration_kind( Node_id decl ) const;
    Node_id                  member_named( Node_id type, Symbol_id name ) const;
    Node_id                  member_after_dot( Node_id id ) const;
    Node_id                  chosen( Node_id use, Node_id found ) const;
    Bindings                 bindings_at( Node_id use ) const;
    Span                     name_span( Node_id id, bool declares ) const;
    Span                     declared_name_span( Node_id decl ) const;
    bool                     is_qualified( Node_id id ) const;

    const Ast&            ast_;
    const Resolution&     resolution_;
    const Types&          types_;
    const Source_manager& sm_;
    const Interner&       interner_;

    std::unordered_set<u32>          top_level_;
    std::unordered_map<u32, Node_id> called_; // a call's callee, to the callable the checker chose
    std::unordered_map<u32, Node_id> calls_;  // a call's callee, to the call
    std::unordered_map<u32, Node_id> owners_; // a type parameter, to the declaration it parameterises
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

        if( ast_.kind( id ) == Node_kind::Call_expr )
        {
            called_.emplace( ast_.callee( id ).v, types_.callee_of( id ) );
            calls_.emplace( ast_.callee( id ).v, id );
        }

        // A method lists its aggregate's parameters too; the aggregate comes after it, and wins.
        for( const Node_id param : ast_.type_parameters( ast_.type_param_list( id ) ) )
        {
            owners_.insert_or_assign( param.v, id );
        }
    }

    for( u32 i = 0; i < ast_.node_count(); ++i )
    {
        const Node_id id { i };

        const bool                     declares = declares_here( id );
        const Node_id                  decl     = declares ? id : referent( id );
        const std::optional<Name_kind> kind     = is_package( id ) ? Name_kind::Package : declaration_kind( decl );

        // Synthesised nodes carry a borrowed span, which the name's text will not match.
        if( const Span span = kind ? name_span( id, declares ) : Span {}; span.is_valid() )
        {
            if( !decl.is_valid() )
            {
                names.push_back( { span, *kind, Span {} } );
                continue;
            }

            const auto owner = owners_.find( decl.v );

            names.push_back(
                { span,
                  *kind,
                  declared_name_span( decl ),
                  print_signature( ast_, resolution_, types_, interner_, decl, bindings_at( id ) ),
                  documentation( ast_, sm_, decl ),
                  owner != owners_.end() ? print_signature( ast_, resolution_, types_, interner_, owner->second ) : "" }
            );
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

// A declaration whose own name is written here; a type parameter and a pattern binding are named
// through `referent`, and a constructor or destructor by its type.
bool Name_collector::declares_here( Node_id id ) const
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Function_decl:
    case Node_kind::Method_decl:
    case Node_kind::Struct_decl:
    case Node_kind::Class_decl:
    case Node_kind::Enum_decl:
    case Node_kind::Variant_decl:
    case Node_kind::Field_decl:
    case Node_kind::Var_decl:
    case Node_kind::Param_decl:
        return ast_.name( id ).is_valid() && ast_.name( id ) != Interner::keyword( Keyword::This );
    default:
        return false;
    }
}

// The declaration a written name refers to, or invalid when it is not a name or resolved to nothing.
Node_id Name_collector::referent( Node_id id ) const
{
    const Node_id decl = resolution_.declaration_of( id );

    switch( ast_.kind( id ) )
    {
    case Node_kind::Name_expr:
        return ast_.name( id ) == Interner::keyword( Keyword::This ) ? Node_id {} : chosen( id, decl );
    case Node_kind::Named_type:
    case Node_kind::Struct_literal:
        return decl;
    case Node_kind::Path_expr:
    {
        if( decl.is_valid() )
        {
            return chosen( id, decl );
        }

        // `Colour::Red` and `Buffer::of`: the checker binds these, so the type is asked here.
        const Node_id type = resolution_.declaration_of( ast_.qualifier( id ) );
        return type.is_valid() ? chosen( id, member_named( type, ast_.name( id ) ) ) : Node_id {};
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
    return aggregate.is_valid() ? chosen( id, member_named( aggregate, ast_.name( id ) ) ) : Node_id {};
}

// A callee found by name is the first of its overload set; the checker's choice replaces it. Only a
// function's or method's: a constructor call names its type, and a call through a pointer its variable.
Node_id Name_collector::chosen( Node_id use, Node_id found ) const
{
    if( !found.is_valid() ||
        ( ast_.kind( found ) != Node_kind::Function_decl && ast_.kind( found ) != Node_kind::Method_decl ) )
    {
        return found;
    }

    const auto call = called_.find( use.v );
    return call != called_.end() && call->second.is_valid() ? call->second : found;
}

// What the use binds its declaration's type parameters to: the instance a generic call reached, the
// object's after a `.`, or the enum a variant builds. None inside the generic itself, where `T` is `T`.
Bindings Name_collector::bindings_at( Node_id use ) const
{
    const auto call = calls_.find( use.v );

    if( call != calls_.end() )
    {
        if( const auto which = types_.instantiation_of( call->second ) )
        {
            const Instantiation&       instance = types_.instantiations()[*which];
            const std::vector<Node_id> params   = ast_.type_parameters( ast_.type_param_list( instance.declaration ) );

            Bindings bindings;
            for( std::size_t i = 0; i < params.size() && i < instance.arguments.size(); ++i )
            {
                bindings.emplace( types_.type_of( params[i] ).v, instance.arguments[i] );
            }
            return bindings;
        }
    }

    Type_id instance {};

    if( ast_.kind( use ) == Node_kind::Field_expr )
    {
        instance = types_.type_of( ast_.object( use ) );

        if( instance.is_valid() && types_.table().is_pointer( instance ) )
        {
            instance = types_.table().get( instance ).element;
        }
    }
    else if( ast_.kind( use ) == Node_kind::Path_expr )
    {
        instance = types_.type_of( call != calls_.end() ? call->second : use );
    }

    return aggregate_bindings( ast_, types_.table(), instance, types_.recorded() );
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

// The parser records a declaration's name span, ends a qualified name's span on the name, and starts
// every other one with it.
Span Name_collector::name_span( Node_id id, bool declares ) const
{
    if( ast_.kind( id ) == Node_kind::Struct_literal )
    {
        return ast_.type_name_span( id );
    }

    const std::string_view name = interner_.text( ast_.name( id ) );
    const Span             node = declares ? ast_.name_span( id ) : ast_.span( id );
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
                << decl_start.line << R"(,"decl_col":)" << decl_start.col << R"(,"decl_end_col":)" << decl_end.col
                << R"(,"signature":")" << json_escape( name.signature ) << '"';

            if( !name.owner.empty() )
            {
                out << R"(,"owner":")" << json_escape( name.owner ) << '"';
            }

            if( !name.doc.empty() )
            {
                out << R"(,"doc":")" << json_escape( name.doc ) << '"';
            }
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

// Each name with a declaration as "text@offset: signature", in source order.
std::vector<std::string> signed_names( std::string_view source )
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;

    const File_id    file       = sm.add_file( "t.kl", std::string( source ) );
    const Ast        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
    const Resolution resolution = resolve( ast, sm, interner, diags );
    const Types      types      = type_check( ast, resolution, literals, sm, interner, diags );

    REQUIRE_FALSE( diags.has_errors() );

    std::vector<std::string> out;
    for( const Name& name : collect_names( ast, resolution, types, sm, interner ) )
    {
        out.push_back( fmt::format( "{}@{}: {}", sm.text( name.span ), name.span.start, name.signature ) );
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

    // The class's own name, the constructor's, the annotation and the call.
    REQUIRE( std::ranges::count( names, std::string( "meter:class" ) ) == 4 );
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

        REQUIRE(
            std::ranges::count( names, std::string( "x:field" ) ) == 6
        ); // its declaration, three bare in the methods, p.x and q.x
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

// Every spelling of a call names the overload the checker chose, never the first one declared.
TEST_CASE( "names_point_at_the_overload_chosen", "[sema][names]" )
{
    constexpr std::string_view source = "i32 f( i64 v ) { return 1; }\n"
                                        "i32 f( bool v ) { return 2; }\n"
                                        "struct N\n"
                                        "{\n"
                                        "    i32 x;\n"
                                        "    static i32 g( i64 v ) { return 1; }\n"
                                        "    static i32 g( bool v ) { return 2; }\n"
                                        "    i32 h() const { return g( true ); }\n"
                                        "};\n"
                                        "i32 main() { return f( true ) + N::g( true ); }\n";

    const auto names = named( source, true );
    const auto at    = [&]( std::string_view text, std::size_t skip = 0 )
    { return static_cast<u32>( source.find( text ) + skip ); };

    const u32 f_bool = at( "f( bool v )" );
    const u32 g_bool = at( "g( bool v )" );

    SECTION( "a free function" )
    {
        REQUIRE( has( names, fmt::format( "f@{}->{}", at( "f( true )" ), f_bool ) ) );
    }

    SECTION( "a static method through its type" )
    {
        REQUIRE( has( names, fmt::format( "g@{}->{}", at( "N::g( true )", 3 ), g_bool ) ) );
    }

    SECTION( "a bare sibling call in a method" )
    {
        REQUIRE( has( names, fmt::format( "g@{}->{}", at( "g( true ); }" ), g_bool ) ) );
    }
}

// Hover's text: at an instance every type parameter reads as its argument; inside the generic, `T` stays.
TEST_CASE( "names_carry_the_signature_their_use_sees", "[sema][names]" )
{
    constexpr std::string_view source =
        "struct Box<T>\n"
        "{\n"
        "    T* item;\n"
        "    void put( T* p ) { item = p; }\n"
        "    void again( T* p ) { put( p ); }\n"
        "};\n"
        "enum maybe<T> { some( T value ), none };\n"
        "T first<T>( T a, T b ) where T : Copyable { return a; }\n"
        "void f( Box<i32> b, i32* p ) { b.put( p ); i64 n = first( cast<i64>( 1 ), cast<i64>( 2 ) ); "
        "maybe<bool> m = maybe::some( true ); }\n";

    const auto names = signed_names( source );
    const auto at    = [&]( std::string_view text, std::size_t skip = 0 ) { return source.find( text ) + skip; };

    SECTION( "a method through an instance" )
    {
        REQUIRE( has( names, fmt::format( "put@{}: public void put( i32* p )", at( "b.put", 2 ) ) ) );
    }

    SECTION( "a method inside its generic" )
    {
        REQUIRE( has( names, fmt::format( "put@{}: public void put( T* p )", at( "put( p );" ) ) ) );
    }

    SECTION( "a generic function at the call's instance" )
    {
        REQUIRE( has( names, fmt::format( "first@{}: i64 first<i64>( i64 a, i64 b )", at( "first( cast" ) ) ) );
    }

    SECTION( "a variant at the enum it builds" )
    {
        REQUIRE( has( names, fmt::format( "some@{}: some( bool value )", at( "maybe::some", 7 ) ) ) );
    }

    SECTION( "a variable as declared" )
    {
        REQUIRE( has( names, fmt::format( "b@{}: Box<i32> b", at( "b.put" ) ) ) );
    }
}

// Hovering a declaration shows it as its uses do; a type parameter shows what it parameterises.
TEST_CASE( "names_name_each_declaration_where_written", "[sema][names]" )
{
    constexpr std::string_view source = "i32 total = 0;\n"
                                        "class Box<T> where T : Copyable\n"
                                        "{\n"
                                        "    T item;\n"
                                        "    public static Box<T> of( T v ) { return Box<T>( v ); }\n"
                                        "    public Box( T v ) { item = v; }\n"
                                        "};\n"
                                        "enum Colour { Red( i32 shade ) };\n";

    const auto names = signed_names( source );
    const auto at    = [&]( std::string_view text, std::size_t skip = 0 ) { return source.find( text ) + skip; };

    REQUIRE( has( names, fmt::format( "total@{}: i32 total", at( "total" ) ) ) );
    REQUIRE( has( names, fmt::format( "Box@{}: class Box<T> where T : Copyable", at( "Box" ) ) ) );
    REQUIRE( has( names, fmt::format( "item@{}: private T item", at( "item" ) ) ) );
    REQUIRE( has( names, fmt::format( "of@{}: public static Box<T> of( T v )", at( "of(" ) ) ) );
    REQUIRE( has( names, fmt::format( "v@{}: T v", at( "T v )", 2 ) ) ) );
    REQUIRE( has( names, fmt::format( "Colour@{}: enum Colour", at( "Colour" ) ) ) );
    REQUIRE( has( names, fmt::format( "Red@{}: Red( i32 shade )", at( "Red" ) ) ) );
    REQUIRE( has( names, fmt::format( "shade@{}: i32 shade", at( "shade" ) ) ) );

    SECTION( "a type parameter carries its owner" )
    {
        Source_manager sm;
        Interner       interner;
        Literal_pool   literals;
        Diagnostics    diags;

        const File_id    file       = sm.add_file( "t.kl", std::string( source ) );
        const Ast        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
        const Resolution resolution = resolve( ast, sm, interner, diags );
        const Types      types      = type_check( ast, resolution, literals, sm, interner, diags );

        for( const Name& name : collect_names( ast, resolution, types, sm, interner ) )
        {
            const bool is_type_parameter = name.kind == Name_kind::Type_parameter;
            REQUIRE( ( name.owner == "class Box<T> where T : Copyable" ) == is_type_parameter );
        }
    }
}

TEST_CASE( "names_carry_their_declaration's_doc", "[sema][names]" )
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;

    const File_id file = sm.add_file(
        "t.kl",
        "/// Twice `v`.\n///\n/// Exactly.\ni32 twice( i32 v ) { return v * 2; }\n"
        "i32 g() { return twice( 1 ); }"
    );
    const Ast        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
    const Resolution resolution = resolve( ast, sm, interner, diags );
    const Types      types      = type_check( ast, resolution, literals, sm, interner, diags );

    const auto names = collect_names( ast, resolution, types, sm, interner );
    const auto call =
        std::ranges::find_if( names, [&]( const Name& n ) { return sm.text( n.span ) == "twice" && n.span.start > 40; } );

    REQUIRE( call != names.end() );
    REQUIRE( call->signature == "i32 twice( i32 v )" );
    REQUIRE( call->doc == "Twice `v`.\n\nExactly." );
}

TEST_CASE( "names_struct_literals_by_their_type", "[sema][names]" )
{
    const auto names = named( "struct Point { i32 x; i32 y; };\nvoid f() { Point p = Point { 1, 2 }; }" );

    REQUIRE( std::ranges::count( names, std::string( "Point:struct" ) ) == 3 );
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
        { { Span { file, 9, 10 }, Name_kind::Global, Span { file, 4, 5 }, "i32 x", "The \"x\".\nSecond.", "struct S" },
          { Span { file, 7, 8 }, Name_kind::Package, Span {} } },
        out
    );

    REQUIRE(
        out.str() == "{\"kind\":\"name\",\"refers_to\":\"global\",\"file\":\"a.kl\",\"line\":2,\"col\":3,\"end_col\":4,"
                     "\"decl_file\":\"a.kl\",\"decl_line\":1,\"decl_col\":5,\"decl_end_col\":6,"
                     "\"signature\":\"i32 x\",\"owner\":\"struct S\",\"doc\":\"The \\\"x\\\".\\nSecond.\"}\n"
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
