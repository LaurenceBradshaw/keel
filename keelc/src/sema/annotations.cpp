#include "sema/annotations.h"
#include <fmt/format.h>
#include "sema/type_checker.h"

namespace keel
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

namespace sema
{

Type_id Annotations::type_of( Node_id annotation, bool outermost )
{
    // An invalid Node_id is `auto`, not a mistake - the parser writes one deliberately.
    if( !annotation.is_valid() )
    {
        return Type_id {};
    }

    switch( ast_.kind( annotation ) )
    {
    case Node_kind::Named_type:
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
                    ast_.span( annotation ),
                    fmt::format( "`{}` is not a type", interner_.text( Symbol_id { ast_.aux( annotation ) } ) )
                );

                return table_.builtin( Type_kind::Error );
            }

            // The mirror of a generic call written without its type arguments: what `Box` names on
            // its own is the open form, which no value can have. Inference is a decision of its own
            // and is not taken here, so this names the explicit form rather than guessing.
            if( ( is_aggregate( ast_.kind( decl ) ) || ast_.kind( decl ) == Node_kind::Enum_decl ) && is_generic( ast_, decl ) )
            {
                const std::string_view name = interner_.text( Symbol_id { ast_.aux( annotation ) } );

                reporter_.error_at(
                    ast_.span( annotation ),
                    fmt::format( "`{}` is generic, so its type arguments must be written", name ),
                    fmt::format( "as in `{}<i32>`", name )
                );

                return table_.builtin( Type_kind::Error );
            }

            return types_.type_of( decl );
        }

        const std::string_view spelling = interner_.text( Symbol_id { ast_.aux( annotation ) } );
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

    case Node_kind::Const_type:
        // `const` binds the declaration, not the data, so one that is not outermost is a pointer to
        // const. Refused rather than quietly given the other meaning: silently reinterpreting valid
        // C++ is the one divergence §5.1 forbids.
        if( !outermost )
        {
            reporter_.error_at(
                ast_.span( annotation ),
                "a pointer to `const` is not supported yet",
                "`const` applies to the binding; write `const ref T` to borrow one value read-only"
            );

            return table_.builtin( Type_kind::Error );
        }

        return type_of( ast_.child( annotation, 0 ), false );

    case Node_kind::Pointer_type:
    {
        const Type_id element = type_of( ast_.child( annotation, 0 ), false );

        // Poison propagates rather than being wrapped: `<error>*` is not the error type, so check()
        // would not absorb it and one bad annotation would report twice.
        return table_.is_error( element ) ? element : table_.pointer_to( element );
    }

    // D31/D32: a mode is not a type. It is unwrapped here and nowhere else, so what gets recorded
    // on the declaration is the underlying type and nothing downstream meets the wrapper - anything
    // needing the mode reads it back off the annotation, which is what parameter_mode does.
    case Node_kind::Mode_type:
    {
        const Keyword mode = static_cast<Keyword>( ast_.aux( annotation ) );

        const Type_id inner = type_of( ast_.child( annotation, 0 ), false );

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

    // The mirror of a generic call, asking the same three questions through the same helper.
    case Node_kind::Generic_type:
    {
        const Node_id base      = ast_.child( annotation, 0 );
        const Node_id type_args = ast_.child( annotation, 1 );

        if( !base.is_valid() || !type_args.is_valid() )
        {
            return table_.builtin( Type_kind::Error );
        }

        const Node_id decl = resolution_.declaration_of( base );

        if( !decl.is_valid() )
        {
            // The base resolved to nothing. Reporting here rather than through the Named_type case
            // keeps the span on the whole `Box<i32>`, which is what the author wrote.
            reporter_.error_at(
                ast_.span( annotation ), fmt::format( "unknown type `{}`", interner_.text( Symbol_id { ast_.aux( base ) } ) )
            );

            return table_.builtin( Type_kind::Error );
        }

        const std::string_view name = interner_.text( Symbol_id { ast_.aux( base ) } );

        if( !is_aggregate( ast_.kind( decl ) ) && ast_.kind( decl ) != Node_kind::Enum_decl )
        {
            reporter_.error_at( ast_.span( annotation ), fmt::format( "`{}` is not a generic", name ) );
            return table_.builtin( Type_kind::Error );
        }

        if( !is_generic( ast_, decl ) )
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
            const Node_id child_annotation = ast_.child( decl, 1 );
            Type_id       underlying = child_annotation.is_valid() ? type_of( child_annotation ) : table_.integer( 32, true );
            instance                 = table_.enumeration( decl, arguments, name, underlying );
        }
        else
        {
            instance = table_.structure( decl, arguments, name );
        }

        return instance;
    }

    default:
        // Error nodes, and anything the parser puts in type position that is not a type.
        return table_.builtin( Type_kind::Error );
    }
}

// Shared because a generic aggregate asks exactly what a generic call asks - how many, of what,
// and do they keep the promises - and two copies of that would drift the first time either grew a
// rule.
bool Annotations::resolve_type_arguments(
    Node_id declaration, Node_id type_args, std::string_view name, std::vector<Type_id>& resolved
)
{
    const std::vector<Node_id>     parameters = type_parameters( ast_, ast_.type_param_list( declaration ) );
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
            )
        );

        return false;
    }

    // Non-empty when a call with several candidates has already resolved them - once, because
    // resolving per candidate would report an unknown type once per candidate.
    if( resolved.empty() )
    {
        resolved.reserve( parameters.size() );

        for( const Node_id argument : given )
        {
            resolved.push_back( type_of( argument ) );
        }
    }

    for( std::size_t i = 0; i < parameters.size(); ++i )
    {
        bounds_.check_bounds( parameters[i], ast_.span( given[i] ), resolved[i], name );
    }

    return true;
}

} // namespace sema
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

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

            for( const Node_id type_param : type_parameters( ast_, ast_.type_param_list( decl ) ) )
            {
                arguments.push_back( types_.type_of( type_param ) );
            }

            types_.record(
                decl, types_.table().structure( decl, arguments, interner_.text( Symbol_id { ast_.aux( decl ) } ) )
            );
        }

        aggregates_.order_structs();
        aggregates_.compute_owning();

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

    REQUIRE( p.annotations().type_of( p.parameter_annotation( 0, 0 ) ) == p.table().integer( 32, true ) );

    // `auto` is an invalid Node_id the parser writes deliberately, not a mistake to report.
    REQUIRE_FALSE( p.annotations().type_of( Node_id {} ).is_valid() );
    REQUIRE( p.errors() == 0 );
}

TEST_CASE( "annotations_refuse_a_name_that_is_not_a_type", "[sema][annotation]" )
{
    Written p( "i32 f() { return 0; }\nvoid g( f x ) { }" );

    const Type_id type = p.annotations().type_of( p.parameter_annotation( 1, 0 ) );

    INFO( p.rendered() );
    REQUIRE( p.table().is_error( type ) );
    REQUIRE( p.rendered().find( "`f` is not a type" ) != std::string::npos );
}

TEST_CASE( "annotations_refuse_a_generic_named_without_its_arguments", "[sema][annotation][generic]" )
{
    Written p( "struct Box<T> { T v; };\nvoid g( Box b ) { }" );

    const Type_id type = p.annotations().type_of( p.parameter_annotation( 1, 0 ) );

    INFO( p.rendered() );
    REQUIRE( p.table().is_error( type ) );
    REQUIRE( p.rendered().find( "`Box` is generic, so its type arguments must be written" ) != std::string::npos );
}

TEST_CASE( "annotations_propagate_the_poison_out_of_a_pointer", "[sema][annotation]" )
{
    Written p( "void g( Nope* q ) { }" );

    const Type_id type = p.annotations().type_of( p.parameter_annotation( 0, 0 ) );

    INFO( p.rendered() );

    // The error type itself, not a pointer to it: `<error>*` is not the error type, so check()
    // would not absorb it and one bad annotation would report twice.
    REQUIRE( type == p.table().builtin( Type_kind::Error ) );
    REQUIRE( p.errors() == 1 );
}

TEST_CASE( "annotations_take_const_off_the_outermost_and_refuse_it_inside", "[sema][annotation][const]" )
{
    Written p( "void g( const i32 a, const i32* q ) { }" );

    SECTION( "outermost is the binding, and unwraps to what it binds" )
    {
        REQUIRE( p.annotations().type_of( p.parameter_annotation( 0, 0 ) ) == p.table().integer( 32, true ) );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "inside a pointer it is a promise about data nobody named here" )
    {
        const Type_id type = p.annotations().type_of( p.parameter_annotation( 0, 1 ) );

        INFO( p.rendered() );
        REQUIRE( p.table().is_error( type ) );
        REQUIRE( p.rendered().find( "a pointer to `const` is not supported yet" ) != std::string::npos );
    }
}

TEST_CASE( "annotations_unwrap_a_mode_and_refuse_out_for_an_owning_type", "[sema][annotation][owning]" )
{
    SECTION( "a copyable one is unwrapped to the type underneath" )
    {
        Written p( "void g( out i32 v ) { }" );

        REQUIRE( p.annotations().type_of( p.parameter_annotation( 0, 0 ) ) == p.table().integer( 32, true ) );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "an owning one would leak what the caller was already holding" )
    {
        Written p( "class R { i32 x; ~R() { } };\nvoid g( out R r ) { }" );

        const Type_id type = p.annotations().type_of( p.parameter_annotation( 1, 0 ) );

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

    const Type_id type = p.annotations().type_of( p.parameter_annotation( 1, 0 ) );

    INFO( p.rendered() );
    REQUIRE( p.table().is_error( type ) );
    REQUIRE( p.rendered().find( "`Plain` is not a generic" ) != std::string::npos );
}

TEST_CASE( "annotations_make_the_whole_type_an_error_when_an_argument_is", "[sema][annotation][generic]" )
{
    Written p( "struct Box<T> { T v; };\nvoid g( Box<Nope> b ) { }" );

    const Type_id type = p.annotations().type_of( p.parameter_annotation( 1, 0 ) );

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

// A pointer to const is a promise about data nobody named here, which needs constness in the type
// rather than on the binding. Refused rather than quietly given the other meaning: silently
// reinterpreting valid C++ is the one divergence §5.1 forbids.
TEST_CASE( "type_checker_refuses_a_pointer_to_const", "[sema][const]" )
{
    const Typed p( "i32 main() { i32 y = 1; const i32* q = &y; return *q; }" );

    INFO( p.rendered() );
    REQUIRE( p.rendered().find( "a pointer to `const` is not supported yet" ) != std::string::npos );

    // Exactly one. `<error>*` is not the error type, so wrapping the poison rather than propagating
    // it made one bad annotation report twice.
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

} // namespace
} // namespace keel
#endif
