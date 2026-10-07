// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/annotations.h"
#include <fmt/format.h>
#include "sema/type_checker.h"

namespace keel::sema
{

namespace
{

// D1. The rejected C++ spellings are ordinary identifiers to the lexer, so this is the first point
// that knows one was written in type position - which is exactly the argument for putting the
// suggestion here rather than in lex/. Empty when there is nothing to suggest.
//
// Only single-word spellings can appear: `unsigned int` and `long long` are two identifiers and
// fail in the parser long before sema sees them.
std::string_view keel_spelling_for( std::string_view cpp_spelling )
{
    struct Alias
    {
        std::string_view from;
        std::string_view to;
    };

    static constexpr Alias aliases[] = {
        { "int", "i32" },       { "signed", "i32" },   { "unsigned", "u32" },  { "short", "i16" },    { "long", "i64" },
        { "char", "i8" },       { "float", "f32" },    { "double", "f64" },    { "size_t", "u64" },   { "ssize_t", "i64" },
        { "ptrdiff_t", "i64" }, { "intptr_t", "i64" }, { "uintptr_t", "u64" }, { "int8_t", "i8" },    { "int16_t", "i16" },
        { "int32_t", "i32" },   { "int64_t", "i64" },  { "uint8_t", "u8" },    { "uint16_t", "u16" }, { "uint32_t", "u32" },
        { "uint64_t", "u64" },
    };

    for( const Alias& alias : aliases )
    {
        if( alias.from == cpp_spelling )
        {
            return alias.to;
        }
    }

    return {};
}
} // namespace

Type_id Annotations::resolve( Node_id annotation, bool outermost )
{
    // An invalid Node_id is `auto`, not a mistake - the parser writes one deliberately.
    if( !annotation.is_valid() )
    {
        return Type_id {};
    }

    switch( ast_.kind( annotation ) )
    {
    case Node_kind::Named_type:
        return resolve_named( annotation );

    case Node_kind::Const_type:
        // `const` binds the declaration; the one under a * is read by the pointer cases below.
        if( !outermost )
        {
            reporter_.error_at(
                ast_.span( annotation ),
                "`const` here applies to nothing",
                "`const` makes a declaration read-only, or what a pointer points at"
            );

            return table_.builtin( Type_kind::Error );
        }

        return resolve( ast_.inner_type( annotation ), false );

    case Node_kind::Pointer_type:
    case Node_kind::Many_pointer_type:
    {
        const Node_id spelled       = ast_.inner_type( annotation );
        const bool    const_element = ast_.kind( spelled ) == Node_kind::Const_type;
        const Type_id element       = resolve( const_element ? ast_.inner_type( spelled ) : spelled, false );

        if( ast_.kind( annotation ) == Node_kind::Many_pointer_type && table_.is_void( element ) )
        {
            reporter_.error_at(
                ast_.span( annotation ), "`void[*]` has no element to point at", "name the element's type, as in `u8[*]`"
            );

            return table_.builtin( Type_kind::Error );
        }

        // Poison propagates rather than being wrapped: `<error>*` is not the error type, so check()
        // would not absorb it and one bad annotation would report twice.
        if( table_.is_error( element ) )
        {
            return element;
        }

        if( ast_.kind( annotation ) == Node_kind::Pointer_type )
        {
            return table_.pointer_to( element, const_element );
        }
        else
        {
            return table_.many_pointer_to( element, const_element );
        }
    }

    // D31/D32: a mode is not a type. It is unwrapped here and nowhere else, so what gets recorded
    // on the declaration is the underlying type and nothing downstream meets the wrapper - anything
    // needing the mode reads it back off the annotation, which is what parameter_mode does.
    case Node_kind::Mode_type:
    {
        const Keyword mode = ast_.keyword( annotation );

        const Type_id inner = resolve( ast_.inner_type( annotation ), false );

        // An `out` parameter is assigned without its old value being destroyed, so an owning one
        // would leak whatever the caller was already holding. Refused until the caller emits a
        // drop before the call - which drop flags could do, but is its own piece of work.
        if( mode == Keyword::Out && !bounds_.satisfies( inner, Bound::Copyable ) )
        {
            reporter_.error_at(
                ast_.span( annotation ),
                "`out` is not supported for a type that owns a resource yet",
                "the value already there would be overwritten without being destroyed"
            );

            return table_.builtin( Type_kind::Error );
        }

        return inner;
    }

    case Node_kind::Generic_type:
        return resolve_generic( annotation );
    case Node_kind::Function_type:
        return resolve_function_type( annotation );
    case Node_kind::Field_type:
        return resolve_field_type( annotation );
    default:
        // Error nodes, and anything the parser puts in type position that is not a type.
        return table_.builtin( Type_kind::Error );
    }
}

Type_id Annotations::resolve_named( Node_id annotation )
{
    const Node_id decl = resolution_.declaration_of( annotation );

    if( decl.is_valid() )
    {
        // An enum names a type as much as an aggregate does. Not folded into is_aggregate(),
        // which gates the struct/class member rules and has no business answering this.
        if( !is_aggregate( ast_.kind( decl ) ) && ast_.kind( decl ) != Node_kind::Enum_decl &&
            ast_.kind( decl ) != Node_kind::Type_param_decl )
        {
            // Resolved to a function or variable of the same name.
            reporter_.error_at(
                ast_.span( annotation ), fmt::format( "`{}` is not a type", interner_.text( ast_.name( annotation ) ) )
            );

            return table_.builtin( Type_kind::Error );
        }

        // The mirror of a generic call written without its type arguments: what `Box` names on
        // its own is the open form, which no value can have. Inference is a decision of its own
        // and is not taken here, so this names the explicit form rather than guessing.
        if( ( is_aggregate( ast_.kind( decl ) ) || ast_.kind( decl ) == Node_kind::Enum_decl ) && ast_.is_generic( decl ) )
        {
            const std::string_view name = interner_.text( ast_.name( annotation ) );

            reporter_.error_at(
                ast_.span( annotation ),
                fmt::format( "`{}` is generic, so its type arguments must be written", name ),
                fmt::format( "as in `{}<i32>`", name )
            );

            return table_.builtin( Type_kind::Error );
        }

        return types_.type_of( decl );
    }

    // `kl::Missing` was reported by the resolver, `kl::i32` is not `i32`, and an error symbol's
    // declaration was reported where it failed.
    if( ast_.package( annotation ).is_valid() || resolution_.is_unresolved( annotation ) )
    {
        return table_.builtin( Type_kind::Error );
    }

    const std::string_view spelling = interner_.text( ast_.name( annotation ) );
    const Type_id          type     = table_.from_spelling( spelling );

    if( type.is_valid() )
    {
        return type;
    }

    const std::string_view suggestion = keel_spelling_for( spelling );

    reporter_.error_at(
        ast_.span( annotation ),
        fmt::format( "unknown type `{}`", spelling ),
        suggestion.empty() ? std::string {} : fmt::format( "Keel spells this `{}`", suggestion )
    );

    return table_.builtin( Type_kind::Error );
}

// The mirror of a generic call, asking the same three questions through the same helper.
Type_id Annotations::resolve_generic( Node_id annotation )
{
    const Node_id base      = ast_.generic_name( annotation );
    const Node_id type_args = ast_.type_arg_list( annotation );

    if( !base.is_valid() || !type_args.is_valid() )
    {
        return table_.builtin( Type_kind::Error );
    }

    const Node_id decl = resolution_.declaration_of( base );

    // A qualified base was reported by the resolver, and an error symbol where it failed.
    if( !decl.is_valid() && ( ast_.package( base ).is_valid() || resolution_.is_unresolved( base ) ) )
    {
        return table_.builtin( Type_kind::Error );
    }

    if( !decl.is_valid() )
    {
        // The base resolved to nothing. Reporting here rather than through the Named_type case
        // keeps the span on the whole `Box<i32>`, which is what the author wrote.
        reporter_.error_at( ast_.span( annotation ), fmt::format( "unknown type `{}`", interner_.text( ast_.name( base ) ) ) );

        return table_.builtin( Type_kind::Error );
    }

    const std::string_view name = interner_.text( ast_.name( base ) );

    if( !is_aggregate( ast_.kind( decl ) ) && ast_.kind( decl ) != Node_kind::Enum_decl )
    {
        reporter_.error_at( ast_.span( annotation ), fmt::format( "`{}` is not a generic", name ) );
        return table_.builtin( Type_kind::Error );
    }

    if( !ast_.is_generic( decl ) )
    {
        reporter_.error_at(
            ast_.span( annotation ),
            fmt::format( "`{}` is not a generic", name ),
            fmt::format( "it takes no type arguments, so write `{}` on its own", name )
        );

        return table_.builtin( Type_kind::Error );
    }

    std::vector<Type_id> arguments;

    if( !resolve_type_arguments( decl, type_args, name, arguments ) )
    {
        return table_.builtin( Type_kind::Error );
    }

    // An argument that failed to resolve makes the whole type an error rather than interning
    // `Box<<error>>` - which would spell nothing, and which every later diagnostic would name.
    for( const Type_id argument : arguments )
    {
        if( !argument.is_valid() || table_.is_error( argument ) )
        {
            return table_.builtin( Type_kind::Error );
        }
    }

    Type_id instance {};
    if( ast_.kind( decl ) == Node_kind::Enum_decl )
    {
        const Node_id written    = ast_.underlying_type( decl );
        Type_id       underlying = written.is_valid() ? resolve( written ) : table_.integer( 32, true );
        instance                 = table_.enumeration( decl, arguments, name, underlying );
    }
    else
    {
        instance = table_.structure( decl, arguments, name );
    }

    return instance;
}

Type_id Annotations::resolve_function_type( Node_id annotation )
{
    const Node_id spelled_return = ast_.return_type( annotation );
    const Node_id bare_return    = ast_.unwrap_const( spelled_return );
    const Keyword return_mode_kw = ast_.parameter_mode( annotation );

    // The unwrapped node, not what was written: `const ref i32` reaches here as a Const_type,
    // and typing that with outermost false is what reports a pointer to `const` - the right
    // refusal for the wrong reason, and a second diagnostic under every one below.
    //
    // Nothing written inside a function type is a declaration, so none of it is outermost.
    const Type_id return_type = resolve( bare_return, false );
    bool          poisoned    = table_.is_error( return_type );

    Param_mode return_mode = Param_mode::Value;
    if( return_mode_kw == Keyword::Ref && bare_return != spelled_return )
    {
        return_mode = Param_mode::Const_ref;
    }
    else if( return_mode_kw == Keyword::Ref )
    {
        reporter_.error_at(
            ast_.span( spelled_return ),
            "only a `const ref` may be returned",
            fmt::format( "write `const ref {}`", table_.name( return_type ) )
        );
        poisoned = true;
    }
    else if( return_mode_kw == Keyword::Out || return_mode_kw == Keyword::Move )
    {
        const std::string_view mode_text = interner_.text( Interner::keyword( return_mode_kw ) );

        reporter_.error_at(
            ast_.span( spelled_return ),
            fmt::format( "`{}` is not a return mode", mode_text ),
            fmt::format( "`{}` says how an argument travels, and a return is not an argument", mode_text )
        );
        poisoned = true;
    }
    else if( return_mode_kw == Keyword::Count && bare_return != spelled_return )
    {
        reporter_.error_at(
            const_keyword( spelled_return ),
            "a `const` here binds nothing, because a function type has no callee",
            "remove it - whether the callee copies is its own business"
        );
        poisoned = true;
    }

    std::vector<Parameter> parameters;

    for( const Node_id param : ast_.params( annotation ) )
    {
        Parameter     parameter;
        const Node_id spelled = ast_.annotation( param );

        const Node_id bare = ast_.unwrap_const( spelled );

        // A `const` with no mode under it: the only thing it could bind is a callee's own copy,
        // and a type has no callee. `const ref` reaches the Mode_type and is a mode like the rest.
        if( bare != spelled && ast_.kind( bare ) != Node_kind::Mode_type )
        {
            reporter_.error_at(
                const_keyword( spelled ),
                "a `const` here binds nothing, because a function type has no callee",
                "remove it - whether the callee copies is its own business"
            );

            poisoned = true;
            continue;
        }

        parameter.mode = parameter_mode_of( ast_, param );

        const Type_id param_type = resolve( bare, false );

        poisoned = poisoned || table_.is_error( param_type );

        parameter.type = param_type;
        parameters.push_back( parameter );
    }

    // Poison propagates rather than being wrapped, for the reason Pointer_type's does: a
    // `fn( <error> ) -> i32` is not the error type, so one bad annotation would report twice.
    return poisoned ? table_.builtin( Type_kind::Error ) : table_.function( return_type, parameters, return_mode );
}

Type_id Annotations::resolve_field_type( Node_id annotation )
{
    const Node_id spelled_member    = ast_.child( annotation, 0 );
    const Node_id bare_member       = ast_.unwrap_const( spelled_member );
    const Node_id spelled_aggregate = ast_.child( annotation, 1 );
    const Keyword member_mode       = ast_.parameter_mode( annotation );

    // Stripped by hand rather than typed through: a Mode_type reports its own refusals, and a mode here is refused whole.
    Node_id bare_aggregate = ast_.unwrap_const( spelled_aggregate );
    if( ast_.kind( bare_aggregate ) == Node_kind::Mode_type )
    {
        bare_aggregate = ast_.inner_type( bare_aggregate );
    }

    const Type_id aggregate = resolve( bare_aggregate, false );
    bool          poisoned  = table_.is_error( aggregate );

    if( bare_aggregate != spelled_aggregate )
    {
        reporter_.error_at(
            ast_.span( spelled_aggregate ),
            "the aggregate of a field type takes no mode",
            "one offset serves every object of the type, so write the type alone"
        );
        poisoned = true;
    }
    else if( !poisoned && !table_.is_struct( aggregate ) && !table_.is_parameter( aggregate ) )
    {
        reporter_.error_at(
            ast_.span( spelled_aggregate ),
            fmt::format( "`{}` has no fields", table_.name( aggregate ) ),
            "a field type reads a field of a `struct` or a `class`"
        );
        poisoned = true;
    }

    if( member_mode == Keyword::Ref && bare_member != spelled_member )
    {
        reporter_.error_at( ast_.span( spelled_member ), "a field type that borrows is not supported yet" );
        poisoned = true;
    }
    else if( member_mode == Keyword::Ref )
    {
        reporter_.error_at( ast_.span( spelled_member ), "writing through a field type is not supported yet" );
        poisoned = true;
    }
    else if( member_mode == Keyword::Out || member_mode == Keyword::Move )
    {
        const std::string_view mode_text = interner_.text( Interner::keyword( member_mode ) );

        reporter_.error_at(
            ast_.span( spelled_member ),
            fmt::format( "`{}` is not a return mode", mode_text ),
            fmt::format( "`{}` says how an argument travels, and a return is not an argument", mode_text )
        );
        poisoned = true;
    }
    else if( member_mode == Keyword::Count && bare_member != spelled_member )
    {
        reporter_.error_at(
            const_keyword( spelled_member ), "a `const` here binds nothing, because a field type only reads", "remove it"
        );
        poisoned = true;
    }

    // Typed only without a mode, since a Mode_type would add its own refusals under the one above.
    const Type_id member = member_mode == Keyword::Count ? resolve( bare_member, false ) : table_.builtin( Type_kind::Error );

    return poisoned || table_.is_error( member ) ? table_.builtin( Type_kind::Error ) : table_.field( aggregate, member );
}

namespace
{

// `T`, or `T` and `U`, in declaration order - the same joining Overloads uses for a candidate list.
std::string type_parameter_list( const Ast& ast, const Interner& interner, std::span<const Node_id> parameters )
{
    std::string text;

    for( std::size_t i = 0; i < parameters.size(); ++i )
    {
        if( i != 0 )
        {
            text += i + 1 == parameters.size() ? " and " : ", ";
        }

        text += fmt::format( "`{}`", interner.text( ast.name( parameters[i] ) ) );
    }

    return text;
}

} // namespace

// Shared because a generic aggregate asks exactly what a generic call asks - how many, of what,
// and do they keep the promises - and two copies of that would drift the first time either grew a
// rule.
bool Annotations::resolve_type_arguments(
    Node_id declaration, Node_id type_args, std::string_view name, std::vector<Type_id>& resolved
)
{
    const std::vector<Node_id>     parameters = ast_.type_parameters( ast_.type_param_list( declaration ) );
    const std::span<const Node_id> given      = ast_.children( type_args );

    if( parameters.size() != given.size() )
    {
        reporter_.error_at(
            ast_.span( type_args ),
            fmt::format(
                "`{}` takes {} type argument{}, but {} {} given",
                name,
                parameters.size(),
                parameters.size() == 1 ? "" : "s",
                given.size(),
                given.size() == 1 ? "was" : "were"
            ),
            // The names, not the count again: the count is in the message and the names are what
            // the author has to write something for.
            fmt::format( "`{}` declares {}", name, type_parameter_list( ast_, interner_, parameters ) )
        );

        return false;
    }

    // Non-empty when a call with several candidates has already resolved them - once, because
    // resolving per candidate would report an unknown type once per candidate.
    if( resolved.empty() )
    {
        resolved.reserve( parameters.size() );

        // Not outermost: a type argument binds nothing, so `const` on one applies to nothing.
        for( const Node_id argument : given )
        {
            resolved.push_back( resolve( argument, false ) );
        }
    }

    for( std::size_t i = 0; i < parameters.size(); ++i )
    {
        bounds_.check_bounds( parameters[i], ast_.span( given[i] ), resolved[i], name );
    }

    return true;
}

Span Annotations::const_keyword( Node_id const_type ) const
{
    const Span const_type_span = ast_.span( const_type );
    const Span element_span    = ast_.span( ast_.inner_type( const_type ) );

    const u32 const_length = narrow_cast<u32>( interner_.text( Interner::keyword( Keyword::Const ) ).size() );

    if( element_span.start > const_type_span.start )
    {
        return Span { const_type_span.file, const_type_span.start, const_type_span.start + const_length };
    }
    else
    {
        return Span { const_type_span.file, const_type_span.end - const_length, const_type_span.end };
    }
}

} // namespace keel::sema
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>

#include "common/source_manager.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/checker_test_support.h"

namespace keel
{
namespace
{

// The fixture and the cases below name the vocabulary annotations.h declares in `keel::sema`.
using namespace sema;

// The first of these fixtures that has to run the resolver: `declaration_of` is how a Named_type
// finds what it names. Beyond that it does what the declaration pass does for a struct - one struct
// type per aggregate, the type parameters declared, the owning set settled - and nothing else.
class Written
{
public:
    explicit Written( std::string_view source )
        : ast_(
              parse( lex( sm_.add_file( "t.kl", std::string( source ) ), sm_, interner_, literal_pool_, diags_ ), sm_, diags_ )
          ),
          resolution_( resolve( ast_, sm_, interner_, diags_ ) ),
          reporter_( sm_, diags_ ),
          aggregates_( ast_, interner_, types_, reporter_ ),
          bounds_( ast_, interner_, literal_pool_, types_, aggregates_, reporter_ ),
          annotations_( ast_, interner_, resolution_, types_, bounds_, reporter_ )
    {
        types_.size_to( ast_.node_count() );

        for( const Node_id decl : ast_.children( ast_.root() ) )
        {
            bounds_.declare_type_parameters( decl, ast_.type_param_list( decl ) );

            if( !is_aggregate( ast_.kind( decl ) ) )
            {
                continue;
            }

            std::vector<Type_id> arguments;

            for( const Node_id type_param : ast_.type_parameters( ast_.type_param_list( decl ) ) )
            {
                arguments.push_back( types_.type_of( type_param ) );
            }

            types_.record(
                decl, types_.table().structure( decl, arguments, interner_.text( Symbol_id { ast_.aux( decl ) } ) )
            );
        }

        aggregates_.order_structs();

        // Last, so errors() counts only what the case itself provokes.
        earlier_ = diags_.error_count();
    }

    sema::Annotations& annotations()
    {
        return annotations_;
    }

    Type_table& table()
    {
        return types_.table();
    }

    Node_id declaration( std::size_t index ) const
    {
        return ast_.children( ast_.root() )[index];
    }

    // Reached through its declaration rather than by walking the node array for a Param_decl: a
    // destructor contributes a synthesised receiver, which comes first and is not what a case means.
    Node_id parameter_annotation( std::size_t declaration_index, std::size_t index ) const
    {
        const Node_id parameters = ast_.child( declaration( declaration_index ), 1 );

        return ast_.child( ast_.children( parameters )[index], 0 );
    }

    Node_id child( Node_id parent, std::size_t index ) const
    {
        return ast_.children( parent )[index];
    }

    std::size_t errors() const
    {
        return diags_.error_count() - earlier_;
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags_.render( sm_, out );
        return out.str();
    }

private:
    Source_manager sm_;
    Interner       interner_;
    Literal_pool   literal_pool_;
    Diagnostics    diags_;
    Ast            ast_;
    Resolution     resolution_;

    sema::Types_builder types_;
    sema::Reporter      reporter_;
    sema::Aggregates    aggregates_;
    sema::Bounds        bounds_;
    sema::Annotations   annotations_;

    std::size_t earlier_ = 0;
};

TEST_CASE( "annotations_read_a_builtin_and_leave_auto_alone", "[sema][annotation]" )
{
    Written p( "i32 f( i32 a ) { return a; }" );

    REQUIRE( p.annotations().resolve( p.parameter_annotation( 0, 0 ) ) == p.table().integer( 32, true ) );

    // `auto` is an invalid Node_id the parser writes deliberately, not a mistake to report.
    REQUIRE_FALSE( p.annotations().resolve( Node_id {} ).is_valid() );
    REQUIRE( p.errors() == 0 );
}

TEST_CASE( "annotations_refuse_a_name_that_is_not_a_type", "[sema][annotation]" )
{
    Written p( "i32 f() { return 0; }\nvoid g( f x ) { }" );

    const Type_id type = p.annotations().resolve( p.parameter_annotation( 1, 0 ) );

    INFO( p.rendered() );
    REQUIRE( p.table().is_error( type ) );
    REQUIRE( p.rendered().find( "`f` is not a type" ) != std::string::npos );
}

TEST_CASE( "annotations_refuse_a_generic_named_without_its_arguments", "[sema][annotation][generic]" )
{
    Written p( "struct Box<T> { T v; };\nvoid g( Box b ) { }" );

    const Type_id type = p.annotations().resolve( p.parameter_annotation( 1, 0 ) );

    INFO( p.rendered() );
    REQUIRE( p.table().is_error( type ) );
    REQUIRE( p.rendered().find( "`Box` is generic, so its type arguments must be written" ) != std::string::npos );
}

TEST_CASE( "annotations_propagate_the_poison_out_of_a_pointer", "[sema][annotation]" )
{
    Written p( "void g( Nope* q ) { }" );

    const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

    INFO( p.rendered() );

    // The error type itself, not a pointer to it: `<error>*` is not the error type, so check()
    // would not absorb it and one bad annotation would report twice.
    REQUIRE( type == p.table().builtin( Type_kind::Error ) );
    REQUIRE( p.errors() == 1 );
}

TEST_CASE( "annotations_read_a_function_type", "[sema][annotation][m7]" )
{
    Written p( "void g( fn( i32 ) -> bool a, fn() -> void b, fn( fn( i32 ) -> i32 ) -> i32 c ) { }" );

    REQUIRE( p.table().name( p.annotations().resolve( p.parameter_annotation( 0, 0 ) ) ) == "fn( i32 ) -> bool" );
    REQUIRE( p.table().name( p.annotations().resolve( p.parameter_annotation( 0, 1 ) ) ) == "fn() -> void" );
    REQUIRE( p.table().name( p.annotations().resolve( p.parameter_annotation( 0, 2 ) ) ) == "fn( fn( i32 ) -> i32 ) -> i32" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 0 );
}

TEST_CASE( "annotations_read_a_field_type", "[sema][annotation][m7][field]" )
{
    Written p( "struct P { i32 x; };\nvoid g( field( P ) -> i32 a, const field( P ) -> i32 b ) { }" );

    REQUIRE( p.table().name( p.annotations().resolve( p.parameter_annotation( 1, 0 ) ) ) == "field( P ) -> i32" );
    REQUIRE( p.table().name( p.annotations().resolve( p.parameter_annotation( 1, 1 ) ) ) == "field( P ) -> i32" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 0 );
}

TEST_CASE( "annotations_refuse_a_field_type_that_is_not_a_read", "[sema][annotation][m7][field]" )
{
    struct Case
    {
        const char* type;
        const char* message;
    };

    for( const Case c :
         { Case { "field( ref P ) -> i32", "the aggregate of a field type takes no mode" },
           Case { "field( const ref P ) -> i32", "the aggregate of a field type takes no mode" },
           Case { "field( i32 ) -> i32", "`i32` has no fields" },
           Case { "field( P ) -> ref i32", "writing through a field type is not supported yet" },
           Case { "field( P ) -> const ref i32", "a field type that borrows is not supported yet" },
           Case { "field( P ) -> out i32", "`out` is not a return mode" },
           Case { "field( P ) -> const i32", "a `const` here binds nothing, because a field type only reads" },
           Case { "field( Nope ) -> i32", "`Nope`" },
           Case { "field( P ) -> Nope", "`Nope`" } } )
    {
        Written p( fmt::format( "struct P {{ i32 x; }};\nvoid g( {} a ) {{ }}", c.type ) );

        const Type_id type = p.annotations().resolve( p.parameter_annotation( 1, 0 ) );

        INFO( c.type << "\n" << p.rendered() );
        REQUIRE( type == p.table().builtin( Type_kind::Error ) );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( c.message ) != std::string::npos );
    }
}

TEST_CASE( "annotations_propagate_the_poison_out_of_a_function_type", "[sema][annotation][m7]" )
{
    // The error type itself, not a function type wrapping it: `fn( <error> ) -> i32` is not the
    // error type, so check() would not absorb it and one bad annotation would report twice.
    SECTION( "from a parameter" )
    {
        Written p( "void g( fn( Nope ) -> i32 a ) { }" );

        const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

        INFO( p.rendered() );
        REQUIRE( type == p.table().builtin( Type_kind::Error ) );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and from the return type" )
    {
        Written p( "void g( fn( i32 ) -> Nope a ) { }" );

        const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

        INFO( p.rendered() );
        REQUIRE( type == p.table().builtin( Type_kind::Error ) );
        REQUIRE( p.errors() == 1 );
    }
}

TEST_CASE( "annotations_refuse_const_inside_a_function_type", "[sema][annotation][const][m7]" )
{
    SECTION( "the outermost one is still the binding, and unwraps to what it binds" )
    {
        Written p( "void g( const fn( i32 ) -> i32 a ) { }" );

        REQUIRE( p.table().name( p.annotations().resolve( p.parameter_annotation( 0, 0 ) ) ) == "fn( i32 ) -> i32" );
        REQUIRE( p.errors() == 0 );
    }

    // Nothing written inside a function type is a declaration, so `const` there is the pointer
    // meaning, which is refused everywhere else already.
    SECTION( "one in the return position is not" )
    {
        Written p( "void g( fn( i32 ) -> const i32 a ) { }" );

        const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

        INFO( p.rendered() );
        REQUIRE( p.table().is_error( type ) );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "nor is one on a parameter" )
    {
        Written p( "void g( fn( const i32 ) -> i32 a ) { }" );

        const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

        INFO( p.rendered() );
        REQUIRE( p.table().is_error( type ) );
        REQUIRE( p.errors() == 1 );
    }
}

// The caret belongs on the word being refused, not on the type it wraps: `^^^^^` and no wider.
TEST_CASE( "annotations_underline_only_the_const_a_function_type_refuses", "[sema][annotation][const][m7]" )
{
    std::string_view source;
    std::string_view at;

    SECTION( "a leading one on the return" )
    {
        source = "void g( fn( i32 ) -> const i32 a ) { }";
        at     = ":1:22";
    }

    SECTION( "one on a parameter" )
    {
        source = "void g( fn( const i32 ) -> i32 a ) { }";
        at     = ":1:13";
    }

    Written p( source );
    p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

    const std::string rendered = p.rendered();

    INFO( rendered );
    REQUIRE( p.errors() == 1 );
    REQUIRE( rendered.find( at ) != std::string::npos );
    REQUIRE( std::count( rendered.begin(), rendered.end(), '^' ) == 5 );
}

// D31's five parameter forms are five types, because two of them differ in how the call is made
// even where they agree on what the caller writes. `const ref T` and `T` are one overload and two
// types; `ref T` and `out T` are one C spelling and two call markers.
TEST_CASE( "annotations_give_a_function_type_one_type_per_mode", "[sema][annotation][m7][mode]" )
{
    SECTION( "each mode is accepted and spells itself back" )
    {
        for( const char* spelling :
             { "fn( ref i32 ) -> i32", "fn( const ref i32 ) -> i32", "fn( out i32 ) -> i32", "fn( move i32 ) -> i32" } )
        {
            Written p( fmt::format( "void g( {} a ) {{ }}", spelling ) );

            const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

            INFO( spelling << "\n" << p.rendered() );
            REQUIRE( p.errors() == 0 );
            REQUIRE_FALSE( p.table().is_error( type ) );
            REQUIRE( p.table().name( type ) == spelling );
        }
    }

    SECTION( "and the five forms are five types" )
    {
        Written p( "void g( fn( i32 ) -> i32 a, fn( ref i32 ) -> i32 b, fn( const ref i32 ) -> i32 c,\n"
                   "        fn( out i32 ) -> i32 d, fn( move i32 ) -> i32 e ) { }" );

        std::vector<Type_id> seen;

        for( std::size_t i = 0; i < 5; ++i )
        {
            seen.push_back( p.annotations().resolve( p.parameter_annotation( 0, i ) ) );
        }

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );

        for( std::size_t i = 0; i < seen.size(); ++i )
        {
            REQUIRE_FALSE( p.table().is_error( seen[i] ) );

            for( std::size_t j = i + 1; j < seen.size(); ++j )
            {
                REQUIRE( seen[i] != seen[j] );
            }
        }
    }

    // A pointer is a type and a mode is not, so the two stay tellable apart: `i32*` is nullable and
    // reseatable where `ref i32` is neither.
    SECTION( "and a pointer parameter is not a borrow" )
    {
        Written p( "void g( fn( i32* ) -> i32 a, fn( ref i32 ) -> i32 b ) { }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        const Type_id pointer = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );
        const Type_id borrow  = p.annotations().resolve( p.parameter_annotation( 0, 1 ) );

        REQUIRE( pointer != borrow );
    }

    // A by-value `const` is the callee's promise about its own copy, and a type has no callee. It
    // would name the same type as `fn( i32 ) -> i32`, so accepting it gives one type two spellings.
    SECTION( "a bare const binds nothing here" )
    {
        Written p( "void g( fn( const i32 ) -> i32 a ) { }" );

        const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.table().is_error( type ) );
        REQUIRE( p.rendered().find( "binds nothing" ) != std::string::npos );
        REQUIRE( p.rendered().find( "a pointer to `const`" ) == std::string::npos );
    }

    // D32 gives one order, and the parser already says so. The mode being legal now must not add a
    // second diagnostic underneath it.
    SECTION( "const after the mode is an ordering error and nothing else" )
    {
        Written p( "void g( fn( ref const i32 ) -> i32 a ) { }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`const` comes before the mode" ) != std::string::npos );
        REQUIRE( p.rendered().find( "is not supported in a function type" ) == std::string::npos );
        REQUIRE( p.rendered().find( "a pointer to `const`" ) == std::string::npos );
    }

    // D32 again: `const ref T` is one spelling for every position, so the return slot carries it
    // and the mode is part of the type there exactly as it is in a parameter slot.
    SECTION( "and so does one on the return" )
    {
        Written p( "void g( fn( i32 ) -> const ref i32 a ) { }" );

        const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE_FALSE( p.table().is_error( type ) );
        REQUIRE( p.table().name( type ) == "fn( i32 ) -> const ref i32" );
    }

    SECTION( "and a returning borrow is not a returning value" )
    {
        Written p( "void g( fn( i32 ) -> i32 a, fn( i32 ) -> const ref i32 b ) { }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        const Type_id value  = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );
        const Type_id borrow = p.annotations().resolve( p.parameter_annotation( 0, 1 ) );

        REQUIRE( value != borrow );
    }

    // The one reference return the language has is the read-only one, and a type gets the rule and
    // the message a declaration gets: a mutable one would let a caller write through a borrow it
    // never asked for.
    SECTION( "but a mutable borrow may not be returned" )
    {
        Written p( "void g( fn( i32 ) -> ref i32 a ) { }" );

        const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.table().is_error( type ) );
        REQUIRE( p.rendered().find( "only a `const ref` may be returned" ) != std::string::npos );
    }

    // `out` and `move` say how an *argument* travels, and a return is not one.
    SECTION( "and an argument-passing mode is not a return mode" )
    {
        for( const char* mode : { "out", "move" } )
        {
            Written p( fmt::format( "void g( fn( i32 ) -> {} i32 a ) {{ }}", mode ) );

            const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

            INFO( mode << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.table().is_error( type ) );
            REQUIRE( p.rendered().find( fmt::format( "`{}` is not a return mode", mode ) ) != std::string::npos );
        }
    }

    // Same reason the parameter slot gives: a by-value `const` is the callee's promise about its own
    // copy, and a type has no callee - so it would be a second spelling of `fn( i32 ) -> i32`.
    SECTION( "and a bare const binds nothing on the return either" )
    {
        Written p( "void g( fn( i32 ) -> const i32 a ) { }" );

        const Type_id type = p.annotations().resolve( p.parameter_annotation( 0, 0 ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.table().is_error( type ) );
        REQUIRE( p.rendered().find( "binds nothing" ) != std::string::npos );
        REQUIRE( p.rendered().find( "a pointer to `const`" ) == std::string::npos );
    }

    // Unchanged by the slice: the value already there would be overwritten without being destroyed,
    // which is a rule about `out` rather than about where `out` was written.
    SECTION( "and out still refuses a type that owns" )
    {
        Written p( "class B { i32 n; ~B() { } };\nvoid g( fn( out B ) -> i32 a ) { }" );

        // Declaration 1: the class is declaration 0.
        p.annotations().resolve( p.parameter_annotation( 1, 0 ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`out` is not supported for a type that owns a resource yet" ) != std::string::npos );
    }
}

TEST_CASE( "annotations_take_const_off_the_outermost_and_give_it_to_a_pointer", "[sema][annotation][const]" )
{
    Written p( "void g( const i32 a, const i32* q, const u8[*] b, i32* const c, const void* d ) { }" );

    SECTION( "outermost is the binding, and unwraps to what it binds" )
    {
        REQUIRE( p.annotations().resolve( p.parameter_annotation( 0, 0 ) ) == p.table().integer( 32, true ) );
        REQUIRE(
            p.annotations().resolve( p.parameter_annotation( 0, 3 ) ) == p.table().pointer_to( p.table().integer( 32, true ) )
        );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "under a pointer it is the pointer's element" )
    {
        REQUIRE(
            p.annotations().resolve( p.parameter_annotation( 0, 1 ) ) ==
            p.table().pointer_to( p.table().integer( 32, true ), true )
        );
        REQUIRE(
            p.annotations().resolve( p.parameter_annotation( 0, 2 ) ) ==
            p.table().many_pointer_to( p.table().integer( 8, false ), true )
        );
        REQUIRE(
            p.annotations().resolve( p.parameter_annotation( 0, 4 ) ) ==
            p.table().pointer_to( p.table().builtin( Type_kind::Void ), true )
        );
        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }
}

// A type argument binds nothing, so an outer `const` on one protects nothing. It was dropped
// without a word, which made `Box<const i32>` a second spelling of `Box<i32>`.
TEST_CASE( "type_checker_refuses_const_on_a_type_argument", "[sema][const][generic]" )
{
    constexpr std::string_view box = "struct Box<T> where T : Copyable { T v; };\n";

    SECTION( "on a type" )
    {
        const Typed p( std::string( box ) + "i32 take( Box<const i32> b ) { return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`const` here applies to nothing" ) != std::string::npos );
        REQUIRE(
            p.rendered().find( "`const` makes a declaration read-only, or what a pointer points at" ) != std::string::npos
        );
    }

    SECTION( "under a pointer it is the element's" )
    {
        const Typed p( std::string( box ) + "i32 take( Box<const i32*> b ) { return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "annotations_unwrap_a_mode_and_refuse_out_for_an_owning_type", "[sema][annotation][owning]" )
{
    SECTION( "a copyable one is unwrapped to the type underneath" )
    {
        Written p( "void g( out i32 v ) { }" );

        REQUIRE( p.annotations().resolve( p.parameter_annotation( 0, 0 ) ) == p.table().integer( 32, true ) );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "an owning one would leak what the caller was already holding" )
    {
        Written p( "class R { i32 x; ~R() { } };\nvoid g( out R r ) { }" );

        const Type_id type = p.annotations().resolve( p.parameter_annotation( 1, 0 ) );

        INFO( p.rendered() );
        REQUIRE( p.table().is_error( type ) );
        REQUIRE( p.rendered().find( "`out` is not supported for a type that owns a resource yet" ) != std::string::npos );
    }
}

TEST_CASE( "annotations_count_the_type_arguments", "[sema][annotation][generic]" )
{
    Written p( "struct Box<T> { T v; };\nvoid g( Box<i32, f64> b ) { }" );

    std::vector<Type_id> resolved;

    const Node_id type_args = p.child( p.parameter_annotation( 1, 0 ), 1 );

    REQUIRE_FALSE( p.annotations().resolve_type_arguments( p.declaration( 0 ), type_args, "Box", resolved ) );

    INFO( p.rendered() );
    REQUIRE( p.rendered().find( "`Box` takes 1 type argument, but 2 were given" ) != std::string::npos );
}

TEST_CASE( "annotations_keep_the_arguments_a_caller_already_resolved", "[sema][annotation][generic]" )
{
    // A call with several candidates resolves them once. Resolving them again here is how an
    // unknown type gets reported once per candidate.
    Written p( "struct Box<T> { T v; };\nvoid g( Box<Nope> b ) { }" );

    const Node_id type_args = p.child( p.parameter_annotation( 1, 0 ), 1 );

    std::vector<Type_id> resolved { p.table().integer( 32, true ) };

    REQUIRE( p.annotations().resolve_type_arguments( p.declaration( 0 ), type_args, "Box", resolved ) );

    INFO( p.rendered() );
    REQUIRE( resolved.size() == 1 );
    REQUIRE( resolved[0] == p.table().integer( 32, true ) );
    REQUIRE( p.errors() == 0 );
}

TEST_CASE( "annotations_refuse_a_non_generic_given_type_arguments", "[sema][annotation][generic]" )
{
    Written p( "struct Plain { i32 x; };\nvoid g( Plain<i32> b ) { }" );

    const Type_id type = p.annotations().resolve( p.parameter_annotation( 1, 0 ) );

    INFO( p.rendered() );
    REQUIRE( p.table().is_error( type ) );
    REQUIRE( p.rendered().find( "`Plain` is not a generic" ) != std::string::npos );
}

TEST_CASE( "annotations_make_the_whole_type_an_error_when_an_argument_is", "[sema][annotation][generic]" )
{
    Written p( "struct Box<T> { T v; };\nvoid g( Box<Nope> b ) { }" );

    const Type_id type = p.annotations().resolve( p.parameter_annotation( 1, 0 ) );

    INFO( p.rendered() );

    // Rather than interning `Box<<error>>`, which spells nothing and which every later diagnostic
    // would name.
    REQUIRE( type == p.table().builtin( Type_kind::Error ) );
    REQUIRE( p.rendered().find( "unknown type `Nope`" ) != std::string::npos );
}

// D1: the rejected C++ spellings are ordinary identifiers to the lexer, and sema is what knows
// they were used in type position.
TEST_CASE( "type_checker_suggests_the_keel_spelling_for_a_c_type", "[sema][types]" )
{
    const Typed p( "i32 main() { int x = 1; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "i32" ) != std::string::npos );
}

// D1's promise is that a C++ spelling is met with the Keel one, not a bare "unknown type".
TEST_CASE( "d1_suggests_a_keel_spelling_for_every_rejected_c_type", "[sema][types]" )
{
    struct Case
    {
        std::string_view cpp;
        std::string_view keel;
    };

    static const Case cases[] = {
        { "int", "i32" },
        { "unsigned", "u32" },
        { "short", "i16" },
        { "long", "i64" },
        { "char", "i8" },
        { "float", "f32" },
        { "double", "f64" },
        { "size_t", "u64" },
        { "uint8_t", "u8" },
        { "int64_t", "i64" },
    };

    for( const Case& c : cases )
    {
        INFO( c.cpp );
        REQUIRE( keel_spelling_for( c.cpp ) == c.keel );
    }

    SECTION( "and stays quiet when there is nothing to suggest" )
    {
        REQUIRE( keel_spelling_for( "Widget" ).empty() );
        REQUIRE( keel_spelling_for( "" ).empty() );

        // Already Keel spellings: these never reach the suggestion path, but suggesting a type
        // as a replacement for itself would be a bug worth catching if they did.
        REQUIRE( keel_spelling_for( "i32" ).empty() );
        REQUIRE( keel_spelling_for( "bool" ).empty() );
    }
}

// C++'s meaning: `const` on the element is a promise about the data, made through this pointer.
TEST_CASE( "type_checker_accepts_a_pointer_to_const", "[sema][const]" )
{
    const Typed p( "i32 main() { i32 y = 1; const i32* q = &y; return *q; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 1 ) ) == "const i32*" );
}

// `<error>*` is not the error type, so wrapping the poison rather than propagating it would make
// one bad annotation report twice.
TEST_CASE( "type_checker_reports_a_bad_const_element_once", "[sema][const]" )
{
    const Typed p( "i32 main() { const Nope* q = nullptr; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

TEST_CASE( "type_checker_checks_type_arguments", "[sema][generic]" )
{
    const std::string_view id = "T id<T>( T a ) where T : Copyable { return a; }\n";

    SECTION( "too many" )
    {
        const Typed p( std::string( id ) + "i32 main() { return id<i32, f64>( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "takes 1 type argument, but 2 were given" ) != std::string::npos );
    }

    SECTION( "too few" )
    {
        const Typed p( "T pick<T, U>( T a, U b ) where T : Copyable { return a; }\ni32 main() { return pick<i32>( 1, 2 ); }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "type argument" ) != std::string::npos );
    }

    SECTION( "an unknown one is reported once" )
    {
        const Typed p( std::string( id ) + "i32 main() { return id<Nope>( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "unknown type `Nope`" ) != std::string::npos );
    }

    SECTION( "a non-generic given type arguments" )
    {
        // The shape `a < b > ( c )` now parses as, so this is the message that reading produces.
        const Typed p( "i32 f( i32 a ) { return a; }\ni32 main() { return f<i32>( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`f` is not a generic" ) != std::string::npos );
    }

    SECTION( "a generic given none takes them from the expectation" )
    {
        // The literal says nothing - its own type is what is being deduced - so the `i32` the
        // return wants is the only thing here that does.
        const Typed p( std::string( id ) + "i32 main() { return id( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a generic given none, with nothing to deduce from" )
    {
        const Typed p( std::string( id ) + "i32 main() { id( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "nothing here says what `T` is in `id`" ) != std::string::npos );
    }

    SECTION( "a mistake inside an argument is still reported" )
    {
        const Typed p( std::string( id ) + "i32 main() { return id<i32>( nope ); }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "nope" ) != std::string::npos );
    }
}

// D27: `T[*]` is its own type, not a flag on `T*`, so the two never meet without a `cast`.
TEST_CASE( "type_checker_resolves_a_many_item_pointer_annotation", "[sema][many]" )
{
    SECTION( "the spellings name the types they read as" )
    {
        const std::pair<const char*, const char*> cases[] = {
            { "i32[*]", "i32[*]" },
            { "i32*[*]", "i32*[*]" },
            { "i32[*]*", "i32[*]*" },
            { "u8[*][*]", "u8[*][*]" },
            { "i32*[*]*", "i32*[*]*" },
            { "f64[*]", "f64[*]" },
            { "P[*]", "P[*]" },
            { "Box<i32>[*]", "Box<i32>[*]" },
            { "Box<i32[*]>", "Box<i32[*]>" },
        };

        for( const auto& [spelling, name] : cases )
        {
            const Typed p(
                std::string( "struct P { i32 x; };\nstruct Box<T> where T : Copyable { T v; };\n" ) + "i32 main() { " +
                spelling + " q = nullptr; return 0; }"
            );

            INFO( spelling << "\n" << p.rendered() );

            // `Box<i32> q = nullptr` is refused, and is here only to make the generic-element case
            // stand beside the others; every other spelling is a pointer and takes `nullptr`.
            if( std::string_view( spelling ) != "Box<i32[*]>" )
            {
                REQUIRE( p.clean() );
            }
            REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 0 ) ) == name );
        }
    }

    SECTION( "a trailing const is the binding's, and the type is unchanged" )
    {
        const Typed p( "i32 main() { i32[*] const q = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 0 ) ) == "i32[*]" );
    }

    SECTION( "a leading const is the element's: a read-only buffer" )
    {
        const Typed p( "i32 main() { const i32[*] q = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 0 ) ) == "const i32[*]" );
    }

    SECTION( "an unknown element is reported once" )
    {
        const Typed p( "i32 main() { Nope[*] q = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "unknown type `Nope`" ) != std::string::npos );
        REQUIRE( p.errors() == 1 );
    }

    // Indexing needs the element's size, which `void` does not have.
    SECTION( "no `void` element" )
    {
        const Typed p( "i32 main() { void[*] q = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`void[*]` has no element to point at" ) != std::string::npos );
        REQUIRE( p.rendered().find( "name the element's type, as in `u8[*]`" ) != std::string::npos );
    }

    SECTION( "in a signature" )
    {
        const Typed p( "i32[*] pass( i32[*] q ) { return q; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Param_decl, 0 ) ) == "i32[*]" );
    }

    SECTION( "as a field" )
    {
        const Typed p( "struct Buffer { u8[*] data; u64 size; };\n"
                       "i32 main() { Buffer b = Buffer { nullptr, 0 }; u8[*] d = b.data; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_expr, 0 ) ) == "u8[*]" );
    }

    SECTION( "in a function type" )
    {
        const Typed p( "i32[*] pass( i32[*] q ) { return q; }\n"
                       "i32 main() { fn( i32[*] ) -> i32[*] f = &pass; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 0 ) ) == "fn( i32[*] ) -> i32[*]" );
    }
}

} // namespace
} // namespace keel
#endif
