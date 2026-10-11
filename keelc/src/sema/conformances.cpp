// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/conformances.h"
#include <fmt/format.h>
#include <optional>
#include "sema/type_checker.h"

namespace keel
{

namespace
{

Type_id recorded_type( std::span<const Type_id> recorded, Node_id node )
{
    return node.v < recorded.size() ? recorded[node.v] : Type_id {};
}

// The parameters past the receiver, and the return, each with its mode. The receivers differ by
// construction, `Shape` against `Circle`, and constness is asked separately.
bool same_signature( const Ast& ast, std::span<const Type_id> recorded, Node_id requirement, Node_id method )
{
    const std::span<const Node_id> wanted = ast.explicit_params( requirement );
    const std::span<const Node_id> given  = ast.explicit_params( method );

    if( wanted.size() != given.size() || recorded_type( recorded, requirement ) != recorded_type( recorded, method ) ||
        ast.is_ref_parameter( requirement ) != ast.is_ref_parameter( method ) )
    {
        return false;
    }

    for( std::size_t i = 0; i < wanted.size(); ++i )
    {
        if( recorded_type( recorded, wanted[i] ) != recorded_type( recorded, given[i] ) ||
            parameter_mode_of( ast, wanted[i] ) != parameter_mode_of( ast, given[i] ) )
        {
            return false;
        }
    }

    return true;
}

// Nothing when the implementer answers the requirement with `found`; otherwise the problem with
// the nearest candidate. An overload of the name that matches wins over one that does not.
std::optional<Conformance_problem>
answer( const Ast& ast, std::span<const Type_id> recorded, Node_id implementer, Node_id requirement, Node_id& found )
{
    found = Node_id {};

    Conformance_problem nearest = Conformance_problem::Missing;

    for( const Node_id member : ast.members( implementer ) )
    {
        if( ast.kind( member ) != Node_kind::Method_decl || ast.name( member ) != ast.name( requirement ) )
        {
            continue;
        }

        if( !found.is_valid() )
        {
            found = member;
        }

        if( !same_signature( ast, recorded, requirement, member ) )
        {
            nearest = nearest == Conformance_problem::Missing ? Conformance_problem::Mismatched : nearest;
            continue;
        }

        found = member;

        if( ast.is_static_method( member ) )
        {
            return Conformance_problem::Static;
        }

        if( ast.own_type_param_list( member ).is_valid() )
        {
            return Conformance_problem::Generic;
        }

        if( ast.is_const_method( requirement ) && !ast.is_const_method( member ) )
        {
            return Conformance_problem::Not_const;
        }

        if( ast.access( member ) == Access::Private )
        {
            return Conformance_problem::Private;
        }

        return std::nullopt;
    }

    return nearest;
}

} // namespace

const Conformance* Conformances::find( Node_id implementer, Type_id interface ) const
{
    for( const Conformance& conformance : conformances )
    {
        if( conformance.implementer == implementer && conformance.interface == interface )
        {
            return &conformance;
        }
    }

    return nullptr;
}

Node_id Conformances::method_for( Node_id implementer, Node_id requirement ) const
{
    for( const Conformance& conformance : conformances )
    {
        if( conformance.implementer != implementer )
        {
            continue;
        }

        for( std::size_t i = 0; i < conformance.requirements.size(); ++i )
        {
            if( conformance.requirements[i] == requirement )
            {
                return conformance.methods[i];
            }
        }
    }

    return Node_id {};
}

Conformances check_conformances( const Ast& ast, const Type_table& table, std::span<const Type_id> recorded )
{
    Conformances result;

    for( const Node_id decl : ast.declarations( ast.root() ) )
    {
        if( !is_aggregate( ast.kind( decl ) ) )
        {
            continue;
        }

        for( const Node_id written : ast.implemented( decl ) )
        {
            const Type_id interface = recorded_type( recorded, written );

            if( !interface.is_valid() || !table.is_interface( interface ) )
            {
                continue; // reported where it was resolved
            }

            if( result.find( decl, interface ) != nullptr )
            {
                result.errors.push_back(
                    { .problem     = Conformance_problem::Repeated,
                      .at          = ast.span( written ),
                      .implementer = decl,
                      .interface   = interface }
                );
                continue;
            }

            Conformance conformance { .implementer = decl, .interface = interface, .requirements = {}, .methods = {} };

            for( const Node_id requirement : ast.members( table.get( interface ).declaration ) )
            {
                // A static or generic requirement was refused where it was declared; nothing answers one.
                const bool refused = ast.is_static_method( requirement ) || ast.own_type_param_list( requirement ).is_valid();

                Node_id                                  found;
                const std::optional<Conformance_problem> problem = answer( ast, recorded, decl, requirement, found );

                conformance.requirements.push_back( requirement );
                conformance.methods.push_back( problem ? Node_id {} : found );

                if( problem && !refused )
                {
                    result.errors.push_back( {
                        .problem     = *problem,
                        .at          = ast.span( written ),
                        .implementer = decl,
                        .interface   = interface,
                        .requirement = requirement,
                        .found       = found,
                    } );
                }
            }

            result.conformances.push_back( std::move( conformance ) );
        }
    }

    return result;
}

} // namespace keel

namespace keel::sema
{

void report_conformance_errors(
    const Ast&                         ast,
    const Interner&                    interner,
    const Type_table&                  table,
    std::span<const Conformance_error> errors,
    Reporter&                          reporter
)
{
    for( const Conformance_error& error : errors )
    {
        const std::string_view type      = interner.text( ast.name( error.implementer ) );
        const std::string_view interface = table.name( error.interface );

        if( error.problem == Conformance_problem::Repeated )
        {
            reporter.error_at( error.at, fmt::format( "`{}` already implements `{}`", type, interface ) );
            continue;
        }

        const std::string_view method = interner.text( ast.name( error.requirement ) );

        // The requirement as written, without its `;`: what the implementer has to declare.
        std::string_view wanted = reporter.text( ast.span( error.requirement ) );
        if( wanted.ends_with( ';' ) )
        {
            wanted.remove_suffix( 1 );
        }

        switch( error.problem )
        {
        case Conformance_problem::Missing:
            reporter.error_at(
                error.at,
                fmt::format( "`{}` implements `{}` but has no method `{}`", type, interface, method ),
                fmt::format( "add `{}` to `{}`", wanted, type )
            );
            break;
        case Conformance_problem::Mismatched:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` does not match the one `{}` declares", type, method, interface ),
                fmt::format( "`{}` wants `{}`", interface, wanted )
            );
            break;
        case Conformance_problem::Not_const:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` must be `const`, as `{}` declares it", type, method, interface ),
                fmt::format( "a caller holding a `const ref {}` may call it", interface )
            );
            break;
        case Conformance_problem::Static:
            reporter.error_at(
                error.at, fmt::format( "`{}`'s `{}` is `static`, but `{}` calls it on an object", type, method, interface )
            );
            break;
        case Conformance_problem::Generic:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` has type parameters, so it cannot answer `{}`'s", type, method, interface ),
                "a call through the interface reaches one compiled method, which cannot be every instance"
            );
            break;
        case Conformance_problem::Private:
            reporter.error_at(
                error.at,
                fmt::format( "`{}`'s `{}` is private, but `{}` makes it callable by anyone", type, method, interface ),
                fmt::format( "make `{}` public", method )
            );
            break;
        case Conformance_problem::Repeated:
            break;
        }
    }
}

} // namespace keel::sema
#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

TEST_CASE( "conformances_record_the_method_answering_each_requirement", "[sema][interfaces]" )
{
    const Typed p(
        "interface Shape { f64 area() const; void grow( f64 by ); };\n"
        "interface Named { i32 id() const; };\n"
        "struct Sq implements Shape, Named {\n"
        "    f64 s;\n"
        "    void grow( f32 by ) { }\n"
        "    void grow( f64 by ) { }\n"
        "    i32 id() const { return 1; }\n"
        "    f64 area() const { return s * s; }\n"
        "};\n"
        "struct Box<T> implements Shape where T : Copyable { T v; f64 area() const { return 0.0; } void grow( f64 by ) { } };\n"
        "i32 main() { return 0; }"
    );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Ast&          ast          = p.ast();
    const Conformances& conformances = p.types().conformances();
    const Node_id       shape_decl   = p.nth( Node_kind::Interface_decl, 0 );
    const Node_id       named_decl   = p.nth( Node_kind::Interface_decl, 1 );
    const Node_id       square       = p.nth( Node_kind::Struct_decl, 0 );
    const Node_id       box          = p.nth( Node_kind::Struct_decl, 1 );
    const Type_id       shape        = p.types().type_of( shape_decl );
    const Type_id       named        = p.types().type_of( named_decl );

    REQUIRE( conformances.conformances.size() == 3 );
    REQUIRE( conformances.errors.empty() );

    const Conformance* sq_shape = conformances.find( square, shape );
    REQUIRE( sq_shape != nullptr );
    REQUIRE( conformances.find( square, named ) != nullptr );
    REQUIRE( conformances.find( box, shape ) != nullptr ); // keyed by the declaration, not an instance
    REQUIRE( conformances.find( box, named ) == nullptr );

    // In the interface's order, whatever order the implementer declares them in.
    const std::span<const Node_id> requirements = ast.members( shape_decl );
    REQUIRE( sq_shape->requirements == std::vector<Node_id>( requirements.begin(), requirements.end() ) );
    REQUIRE( sq_shape->methods.size() == 2 );

    const std::span<const Node_id> members = ast.members( square );
    REQUIRE( sq_shape->methods[0] == members[4] ); // area
    REQUIRE( sq_shape->methods[1] == members[2] ); // the `grow` that matches, not the first of the name

    REQUIRE( conformances.method_for( square, requirements[1] ) == members[2] );
    REQUIRE( conformances.method_for( square, ast.members( named_decl )[0] ) == members[3] );
    REQUIRE( conformances.method_for( box, requirements[0] ) == ast.members( box )[1] );
    REQUIRE_FALSE( conformances.method_for( square, square ).is_valid() );
}

// Each at the interface as written after `implements`, with the requirement quoted in the help.
TEST_CASE( "conformances_report_each_problem", "[sema][interfaces]" )
{
    struct Case
    {
        const char*         members;
        Conformance_problem problem;
        const char*         message;
        const char*         help;
    };

    const Case cases[] = {
        { "f64 area() const { return s; }",
          Conformance_problem::Missing,
          "`Sq` implements `Shape` but has no method `grow`",
          "add `void grow( f64 by )` to `Sq`" },
        { "f64 area() const { return s; } void grow( f32 by ) { }",
          Conformance_problem::Mismatched,
          "`Sq`'s `grow` does not match the one `Shape` declares",
          "`Shape` wants `void grow( f64 by )`" },
        { "f64 area() const { return s; } void grow( const ref f64 by ) { }",
          Conformance_problem::Mismatched,
          "`Sq`'s `grow` does not match the one `Shape` declares",
          "`Shape` wants `void grow( f64 by )`" },
        { "f64 area() const { return s; } void grow() { }",
          Conformance_problem::Mismatched,
          "`Sq`'s `grow` does not match the one `Shape` declares",
          "`Shape` wants `void grow( f64 by )`" },
        { "f32 area() const { return 0.0; } void grow( f64 by ) { }",
          Conformance_problem::Mismatched,
          "`Sq`'s `area` does not match the one `Shape` declares",
          "`Shape` wants `f64 area() const`" },
        { "f64 area() { return s; } void grow( f64 by ) { }",
          Conformance_problem::Not_const,
          "`Sq`'s `area` must be `const`, as `Shape` declares it",
          "a caller holding a `const ref Shape` may call it" },
        { "static f64 area() { return 0.0; } void grow( f64 by ) { }",
          Conformance_problem::Static,
          "`Sq`'s `area` is `static`, but `Shape` calls it on an object",
          "" },
        { "f64 area() const { return s; } void grow<T>( f64 by ) { }",
          Conformance_problem::Generic,
          "`Sq`'s `grow` has type parameters, so it cannot answer `Shape`'s",
          "a call through the interface reaches one compiled method" },
        { "f64 area() const { return s; } private void grow( f64 by ) { }",
          Conformance_problem::Private,
          "`Sq`'s `grow` is private, but `Shape` makes it callable by anyone",
          "make `grow` public" },
    };

    for( const Case& c : cases )
    {
        const std::string source = std::string( "interface Shape { f64 area() const; void grow( f64 by ); };\n" ) +
                                   "class Sq implements Shape { f64 s; " + c.members + " };\ni32 main() { return 0; }";
        const Typed p( source );

        INFO( c.members << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( c.message ) != std::string::npos );
        REQUIRE( p.rendered().find( c.help ) != std::string::npos );

        const std::vector<Conformance_error>& errors = p.types().conformances().errors;
        REQUIRE( errors.size() == 1 );
        REQUIRE( errors[0].problem == c.problem );
        REQUIRE( errors[0].at.start == source.find( "Shape {", source.find( "implements" ) ) );
        REQUIRE( errors[0].at.len() == 5 );

        // Recorded, so a later stage can still walk every requirement, with nothing where none answers.
        const Conformance* conformance = p.types().conformances().find( errors[0].implementer, errors[0].interface );
        REQUIRE( conformance != nullptr );
        REQUIRE( std::ranges::count_if( conformance->methods, []( Node_id m ) { return !m.is_valid(); } ) == 1 );
    }

    SECTION( "a const method answers one that is not" )
    {
        const Typed p( "interface Shape { void grow( f64 by ); };\n"
                       "struct Sq implements Shape { f64 s; void grow( f64 by ) const { } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a const ref return is part of the signature" )
    {
        const Typed p( "interface Shape { const ref f64 side() const; };\n"
                       "struct Sq implements Shape { f64 s; f64 side() const { return s; } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Sq`'s `side` does not match the one `Shape` declares" ) != std::string::npos );
    }

    SECTION( "every requirement unanswered is reported" )
    {
        const Typed p( "interface Shape { f64 area() const; void grow( f64 by ); };\n"
                       "struct Sq implements Shape { f64 s; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }
}

TEST_CASE( "conformances_refuse_a_repeated_interface", "[sema][interfaces]" )
{
    const std::string source = "interface Shape { f64 area() const; };\n"
                               "struct Sq implements Shape, Shape { f64 s; f64 area() const { return s; } };\n"
                               "i32 main() { return 0; }";
    const Typed       p( source );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "`Sq` already implements `Shape`" ) != std::string::npos );

    const Conformances& conformances = p.types().conformances();
    REQUIRE( conformances.conformances.size() == 1 );
    REQUIRE( conformances.errors[0].at.start == source.find( "Shape {", source.find( "implements" ) ) ); // the second
}

// Each was refused already, so the conformance stage adds nothing to it.
TEST_CASE( "conformances_do_not_cascade_from_an_earlier_error", "[sema][interfaces]" )
{
    const char* cases[] = {
        "interface Shape { static f64 area(); };\nstruct Sq implements Shape { f64 s; };",
        "interface Shape { T pick<T>( T a ); };\nstruct Sq implements Shape { f64 s; };",
        "struct Sq implements i32 { f64 s; };",
        "struct Sq implements Nope { f64 s; };",
    };

    for( const char* c : cases )
    {
        const Typed p( std::string( c ) + "\ni32 main() { return 0; }" );

        INFO( c << "\n" << p.rendered() );
        const std::string rendered = p.rendered();
        std::size_t       reported = 0;
        for( std::size_t at = rendered.find( "error" ); at != std::string::npos; at = rendered.find( "error", at + 1 ) )
        {
            ++reported;
        }

        REQUIRE( reported == 1 ); // the resolver's or the signatures', whichever refused it
        REQUIRE( p.types().conformances().errors.empty() );
    }
}

} // namespace keel
#endif
