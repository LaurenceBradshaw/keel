// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/extern_types.h"

#include <fmt/format.h>

#include "sema/type_checker.h"

namespace keel::sema
{

void Extern_types::check( Node_id extern_decl )
{
    for( const Node_id param : ast_.children( ast_.param_list( extern_decl ) ) )
    {
        if( ast_.kind( param ) != Node_kind::Param_decl )
        {
            continue;
        }

        const Type_id    type = types_.type_of( param );
        const Param_mode mode = parameter_mode_of( ast_, param );
        // At the type, not at `ref`.
        const Node_id spelled = ast_.unwrap_const( ast_.annotation( param ) );
        const Span    at = ast_.span( ast_.kind( spelled ) == Node_kind::Mode_type ? ast_.inner_type( spelled ) : spelled );

        if( !type.is_valid() || table_.is_error( type ) )
        {
            continue;
        }

        if( ( mode == Param_mode::Value || mode == Param_mode::Move ) && table_.is_array( type ) )
        {
            reporter_.error_at(
                at,
                fmt::format( "`{}` cannot be passed to C by value", table_.name( type ) ),
                fmt::format(
                    "C takes an array as a pointer: take `ref {}`, or point at its elements with `{}[*]`",
                    table_.name( type ),
                    table_.name( table_.get( type ).element )
                )
            );
            continue;
        }

        const Foreign found = foreign_parameter( type, mode );
        if( found.part.is_valid() )
        {
            reporter_.error_at(
                at,
                fmt::format(
                    "`{}` cannot be passed to C{}",
                    table_.name( type ),
                    mode == Param_mode::Value || mode == Param_mode::Move ? " by value" : ""
                ),
                help( found )
            );
        }
    }

    const Node_id returned = ast_.return_type( extern_decl );
    const Type_id type     = types_.type_of( extern_decl );
    if( !type.is_valid() || table_.is_error( type ) )
    {
        return;
    }

    const bool borrowed = ast_.parameter_mode( extern_decl ) == Keyword::Ref;
    if( borrowed && table_.is_run( type ) )
    {
        reporter_.error_at(
            ast_.span( returned ),
            "a run cannot be returned from C",
            fmt::format( "return a `{}[*]` and take the count through a `u64*`", table_.name( table_.get( type ).element ) )
        );
        return;
    }

    if( !borrowed && table_.is_array( type ) )
    {
        reporter_.error_at(
            ast_.span( returned ),
            fmt::format( "`{}` cannot be returned from C by value", table_.name( type ) ),
            fmt::format( "C cannot return an array: return a `{}*`", table_.name( table_.get( type ).element ) )
        );
        return;
    }

    const Foreign found = foreign_parameter( type, borrowed ? Param_mode::Const_ref : Param_mode::Value );
    if( found.part.is_valid() )
    {
        reporter_.error_at(
            ast_.span( returned ),
            fmt::format( "`{}` cannot be returned from C{}", table_.name( type ), borrowed ? "" : " by value" ),
            help( found )
        );
    }
}

Extern_types::Foreign Extern_types::foreign_parameter( Type_id type, Param_mode mode ) const
{
    if( mode == Param_mode::Value || mode == Param_mode::Move )
    {
        return foreign_part( type );
    }

    return crosses_by_pointer( type ) ? Foreign {} : Foreign { type, {}, {} };
}

// A borrow is a pointer in C, except to an interface, which is two.
bool Extern_types::crosses_by_pointer( Type_id type ) const
{
    return !table_.is_interface( type );
}

Extern_types::Foreign Extern_types::foreign_part( Type_id type ) const
{
    if( !type.is_valid() || table_.is_error( type ) || table_.is_builtin( type ) || table_.is_void( type ) ||
        table_.is_never( type ) )
    {
        return {};
    }

    if( table_.is_pointer( type ) || table_.is_many_pointer( type ) )
    {
        return table_.is_interface_pointer( type ) ? Foreign { type, {}, {} } : Foreign {};
    }

    if( table_.is_function( type ) )
    {
        return foreign_signature( type );
    }

    const Node_id declaration = table_.get( type ).declaration;

    if( table_.is_enum( type ) )
    {
        return ast_.enum_has_payload( declaration ) ? Foreign { type, {}, {} } : Foreign {};
    }

    if( !table_.is_struct( type ) || ast_.kind( declaration ) != Node_kind::Struct_decl ||
        !table_.get( type ).arguments.empty() || table_.package( type ) == "prelude" )
    {
        return { type, {}, {} };
    }

    // A cycle has its own error, and walking one would never end.
    if( !aggregates_.is_acyclic( declaration ) )
    {
        return {};
    }

    for( const Node_id field : ast_.members( declaration ) )
    {
        if( ast_.kind( field ) != Node_kind::Field_decl )
        {
            continue;
        }

        Type_id field_type = types_.type_of( field );
        if( field_type.is_valid() && table_.is_array( field_type ) )
        {
            field_type = table_.get( field_type ).element;
        }

        Foreign found = foreign_part( field_type );
        if( found.part.is_valid() )
        {
            if( !found.field.is_valid() )
            {
                found.holder = type;
                found.field  = field;
            }
            return found;
        }
    }

    return {};
}

Extern_types::Foreign Extern_types::foreign_signature( Type_id function ) const
{
    const Type& described = table_.get( function );

    for( std::size_t i = 0; i < described.arguments.size(); ++i )
    {
        const Param_mode mode  = i < described.modes.size() ? described.modes[i] : Param_mode::Value;
        const Type_id    param = described.arguments[i];

        if( ( mode == Param_mode::Value || mode == Param_mode::Move ) && table_.is_array( param ) )
        {
            return { param, {}, {} };
        }

        if( const Foreign found = foreign_parameter( param, mode ); found.part.is_valid() )
        {
            return found;
        }
    }

    return foreign_parameter( described.element, described.return_mode );
}

std::string Extern_types::help( const Foreign& found ) const
{
    const Type_id part        = found.part;
    const Node_id declaration = table_.get( part ).declaration;

    std::string_view what   = "laid out by Keel";
    std::string_view remedy = "pass a pointer to it";

    if( table_.is_interface( part ) || table_.is_interface_pointer( part ) )
    {
        what   = "reached through two pointers in C, the object and its table";
        remedy = "pass the implementing type's own pointer";
    }
    else if( table_.is_array( part ) )
    {
        what   = "an array, which C takes as a pointer";
        remedy = "take a pointer to its first element";
    }
    else if( table_.is_union( part ) )
    {
        what   = "an error union, which Keel lays out";
        remedy = "return a status code, or pass a pointer";
    }
    else if( declaration == table_.prelude().str )
    {
        what   = "Keel's text, which Keel lays out";
        remedy = "pass a `const u8[*]` and a `u64`";
    }
    else if( table_.is_struct( part ) && ast_.kind( declaration ) == Node_kind::Class_decl )
    {
        what   = "a class, and C cannot run its destructor";
        remedy = "pass a pointer to it, or a struct of what C needs";
    }
    else if( table_.is_enum( part ) )
    {
        what   = "an enum with a payload, which Keel lays out";
        remedy = "pass a pointer to it, or a C-like enum and the payload's fields apart";
    }
    else if( table_.is_struct( part ) && table_.package( part ) == "prelude" )
    {
        what   = "declared by Keel, which may change its layout";
        remedy = "pass the fields C needs";
    }
    else if( table_.is_struct( part ) && !table_.get( part ).arguments.empty() )
    {
        what   = "a generic instance, which C cannot name";
        remedy = "declare a struct with its fields";
    }

    const std::string subject =
        found.field.is_valid() ? fmt::format( "{}.{}", table_.name( found.holder ), interner_.text( ast_.name( found.field ) ) )
                               : std::string( table_.name( part ) );

    return fmt::format(
        "`{}` is {}{}: {}", subject, found.field.is_valid() ? fmt::format( "`{}`, ", table_.name( part ) ) : "", what, remedy
    );
}

} // namespace keel::sema

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{
namespace
{

// Every extern below is declared and never called, so the only errors are the boundary's.
std::size_t boundary_errors( std::string_view declarations )
{
    const Typed p( std::string( declarations ) + "\ni32 main() { return 0; }\n" );
    INFO( p.rendered() );
    return p.errors();
}

} // namespace

TEST_CASE( "extern_types_accept_what_c_can_read", "[sema][extern]" )
{
    SECTION( "numbers, pointers to anything and parameter runs, as kl_rt's own externs" )
    {
        REQUIRE(
            boundary_errors( "class c { i32 v; ~c() {} };\n"
                             "extern void* f( i32 a, bool b, f64 d, u8[*] p, c* q, void* r );\n"
                             "extern void g( const ref u8[] s, ref i32[] t, u8[*]* out_p );\n"
                             "extern never h();" ) == 0
        );
    }

    SECTION( "a struct of them, an array field included, in and out" )
    {
        REQUIRE(
            boundary_errors( "struct inner { i32 x; u8[4] tag; };\n"
                             "struct outer { inner i; f64 w; };\n"
                             "extern outer f( outer a, move inner b );" ) == 0
        );
    }

    SECTION( "a C-like enum and a function type of C-compatible types" )
    {
        REQUIRE(
            boundary_errors( "enum colour { red, green };\n"
                             "extern colour f( colour c, fn( i32, const ref u8[] ) -> i32 cb );" ) == 0
        );
    }

    SECTION( "a class behind a borrow, and a `const ref` return" )
    {
        REQUIRE(
            boundary_errors( "class c { i32 v; ~c() {} };\n"
                             "extern const ref c f( ref c a, const ref c b, out i32 n );" ) == 0
        );
    }
}

TEST_CASE( "extern_types_refuse_what_c_cannot_read_by_value", "[sema][extern]" )
{
    SECTION( "a class, moved or returned" )
    {
        const Typed p( "class c { i32 v; ~c() {} };\nextern c f( move c a );\ni32 main() { return 0; }\n" );
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "`c` cannot be passed to C by value" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`c` cannot be returned from C by value" ) != std::string::npos );
        REQUIRE( p.rendered().find( "C cannot run its destructor" ) != std::string::npos );
    }

    SECTION( "`str`, with the two parameters that replace it" )
    {
        const Typed p( "extern void f( str s );\ni32 main() { return 0; }\n" );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "pass a `const u8[*]` and a `u64`" ) != std::string::npos );
    }

    SECTION( "a payload enum and an error union" )
    {
        REQUIRE( boundary_errors( "enum shape { circle( f64 r ), dot };\nextern void f( shape s );" ) == 1 );
        REQUIRE( boundary_errors( "enum e1 { a };\nenum e2 { b };\nextern void f( e1 | e2 e );" ) == 1 );
    }

    SECTION( "a generic instance" )
    {
        REQUIRE( boundary_errors( "struct pair<T> where T : copyable { T a; T b; };\nextern void f( pair<i32> p );" ) == 1 );
    }

    SECTION( "an array by value, though a struct may hold one" )
    {
        const Typed p( "extern i32[2] f( i32[2] a );\ni32 main() { return 0; }\n" );
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "point at its elements with `i32[*]`" ) != std::string::npos );
    }

    SECTION( "a returned run" )
    {
        const Typed p( "extern const ref u8[] f();\ni32 main() { return 0; }\n" );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "a run cannot be returned from C" ) != std::string::npos );
    }

    SECTION( "an interface, even behind a pointer or a borrow" )
    {
        REQUIRE(
            boundary_errors( "interface shape { f64 area() const; };\n"
                             "extern void f( shape* a, ref shape b );" ) == 2
        );
    }
}

TEST_CASE( "extern_types_name_the_field_that_holds_the_part", "[sema][extern]" )
{
    const Typed p( "struct inner { str name; };\nstruct outer { inner i; };\n"
                   "extern void f( outer o );\ni32 main() { return 0; }\n" );
    REQUIRE( p.errors() == 1 ); // one per annotation, however deep
    REQUIRE( p.rendered().find( "`outer` cannot be passed to C by value" ) != std::string::npos );
    REQUIRE( p.rendered().find( "`inner.name` is `str`" ) != std::string::npos );
}

TEST_CASE( "extern_types_check_a_function_type_inside_a_signature", "[sema][extern]" )
{
    REQUIRE( boundary_errors( "extern void f( fn( str ) -> i32 cb );" ) == 1 );
    REQUIRE( boundary_errors( "extern void f( fn( i32[2] ) -> i32 cb );" ) == 1 );
}

TEST_CASE( "extern_types_stay_quiet_about_a_cycle_already_reported", "[sema][extern]" )
{
    const Typed p( "struct A { B b; };\nstruct B { A a; };\nextern void f( A a );\ni32 main() { return 0; }\n" );
    INFO( p.rendered() );
    REQUIRE( p.rendered().find( "cannot be passed to C" ) == std::string::npos );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
