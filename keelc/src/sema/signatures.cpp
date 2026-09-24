#include "sema/signatures.h"

#include <fmt/format.h>

#include <unordered_set>

// The signature pass: what each kind of top-level declaration contributes, and the rules over an
// aggregate's own members. The overload rules over those members are `Overloads`'; signatures.h says
// what the pass is for and why its order is the way it is.

namespace keel
{

namespace sema
{

namespace
{

constexpr Member_kind k_member_kinds[] = {
    { Node_kind::Constructor_decl, "constructor", "", "use `class`, or build this from a literal", false },
    { Node_kind::Destructor_decl, "destructor", "~", "use `class` if this type owns a resource", true },
};

} // namespace

void Signatures::declare()
{
    declare_structs();

    // After the struct pass: a payload field may name a struct or a class, and its annotation is
    // resolved here. Before everything else, because a parameter, a field or a global may name an
    // enum in turn.
    declare_enums();
    declare_member_functions();
    declare_fields();
    order_structs();
    check_aggregate_members();
    aggregates_.compute_owning();
    check_struct_ownership();
    check_enum_payloads();
    declare_functions();
    declare_globals();
    places_.record_borrowed_parameters();
    overloads_.check_overload_sets();
}

void Signatures::declare_structs()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( !is_aggregate( ast_.kind( child ) ) )
        {
            continue;
        }

        // Before the type below, because the type *is* its parameters: `Box` on its own is
        // `Box<T>`, the open form D11 checks the body against, and `T` has to exist to name it.
        bounds_.declare_type_parameters( child, ast_.type_param_list( child ) );

        std::vector<Type_id> arguments;

        for( const Node_id type_param : type_parameters( ast_, ast_.type_param_list( child ) ) )
        {
            arguments.push_back( types_.type_of( type_param ) );
        }

        const std::string_view name = interner_.text( Symbol_id { ast_.aux( child ) } );

        types_.record( child, table_.structure( child, arguments, name ) );
    }
}

void Signatures::declare_fields()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( !is_aggregate( ast_.kind( child ) ) )
        {
            continue;
        }

        for( Node_id field : ast_.members( child ) )
        {
            if( ast_.kind( field ) != Node_kind::Field_decl )
            {
                continue;
            }

            const Node_id annotation = ast_.child( field, 0 );

            // A field is not a binding, so `const` has nothing to attach to. Rejected rather than
            // ignored - a keyword that silently does nothing is worse than not having one.
            if( ast_.kind( annotation ) == Node_kind::Const_type )
            {
                reporter_.error_at(
                    ast_.span( annotation ),
                    "a field cannot be `const` yet",
                    "it would have to be written exactly once, during construction, and nothing checks that"
                );
            }

            const Type_id field_type = annotations_.type_of( ast_.child( field, 0 ) );

            types_.record( field, field_type );

            if( is_generic( ast_, child ) )
            {
                generic_recursion_.record_generic_uses( child, field_type, ast_.span( field ) );
            }
        }
    }
}

void Signatures::declare_functions()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Function_decl )
        {
            continue;
        }

        bounds_.declare_type_parameters( child, ast_.type_param_list( child ) );

        const Node_id return_type_node = ast_.child( child, 0 );
        const Type_id return_type      = annotations_.type_of( return_type_node );
        types_.record( child, return_type );

        // §8 allows exactly one reference return, and only the read-only one: a mutable one would
        // let a caller write through a reference it never asked for.
        if( is_ref_parameter( ast_, child ) && !table_.is_error( return_type ) )
        {
            if( !is_const_binding( ast_, child ) )
            {
                reporter_.error_at(
                    ast_.span( return_type_node ),
                    "only a `const ref` may be returned",
                    fmt::format( "write `const ref {}`", table_.name( return_type ) )
                );
            }
            else
            {
                places_.record_binding_address( return_type_node, return_type );
            }
        }

        const Node_id param_list = ast_.child( child, 1 );
        for( Node_id param : ast_.children( param_list ) )
        {
            if( ast_.kind( param ) != Node_kind::Param_decl )
            {
                continue;
            }

            const Node_id param_type_node = ast_.child( param, 0 );
            const Type_id param_type      = annotations_.type_of( param_type_node );
            types_.record( param, param_type );
        }

        if( interner_.text( Symbol_id { ast_.aux( child ) } ) == "main" )
        {
            // Main may not be marked `extern`
            if( is_extern( ast_, child ) )
            {
                reporter_.error_at( ast_.span( child ), "`main` may not be marked `extern`" );
            }

            // return type
            if( !table_.is_error( return_type ) && return_type != table_.integer( 32, true ) )
            {
                reporter_.error_at(
                    ast_.span( return_type_node ),
                    fmt::format( "`main` must return `i32`, but got `{}`", table_.name( return_type ) )
                );
            }

            // parameters
            // Keel currently doesn't support command line args, or arrays,
            // so `main` must be parameterless.
            if( !ast_.children( param_list ).empty() )
            {
                reporter_.error_at( ast_.span( param_list ), "`main` must not take any parameters" );
            }
        }
    }
}

void Signatures::declare_member_functions()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( !is_aggregate( ast_.kind( child ) ) )
        {
            continue;
        }

        for( Node_id member : ast_.members( child ) )
        {
            if( !is_function_like( ast_.kind( member ) ) )
            {
                continue;
            }

            // A constructor and a destructor return nothing and have no annotation to read; a
            // method has both. Child 0 is the return type either way, and is invalid for the two
            // that have none.
            const Node_id return_type_node = ast_.child( member, 0 );

            const Type_id return_type =
                return_type_node.is_valid() ? annotations_.type_of( return_type_node ) : table_.builtin( Type_kind::Void );

            types_.record( member, return_type );

            if( is_generic( ast_, child ) && return_type_node.is_valid() )
            {
                generic_recursion_.record_generic_uses( child, return_type, ast_.span( return_type_node ) );
            }

            // §8 allows exactly one reference return, and only the read-only one - the same rule
            // declare_functions applies to a free function. Without this a method
            // could write `const ref T` and have it silently mean `T`: the recorded address is what
            // every consumer reads to know a binding travels by address, and nothing else sets it.
            if( is_ref_parameter( ast_, member ) && !table_.is_error( return_type ) )
            {
                if( !is_const_binding( ast_, member ) )
                {
                    reporter_.error_at(
                        ast_.span( return_type_node ),
                        "only a `const ref` may be returned",
                        fmt::format( "write `const ref {}`", table_.name( return_type ) )
                    );
                }
                else
                {
                    places_.record_binding_address( return_type_node, return_type );
                }
            }

            for( Node_id param : ast_.children( ast_.child( member, 1 ) ) )
            {
                const Node_id param_type_node = ast_.child( param, 0 );
                const Type_id param_type      = annotations_.type_of( param_type_node );
                types_.record( param, param_type );

                if( is_generic( ast_, child ) )
                {
                    generic_recursion_.record_generic_uses( child, param_type, ast_.span( param_type_node ) );
                }
            }
        }
    }
}

void Signatures::declare_globals()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Var_decl )
        {
            continue;
        }

        const Node_id var_type_node = ast_.child( child, 0 );
        const Type_id var_type      = annotations_.type_of( var_type_node );
        types_.record( child, var_type );
    }
}

void Signatures::declare_enums()
{
    for( Node_id child : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( child ) == Node_kind::Error )
        {
            continue;
        }

        if( ast_.kind( child ) != Node_kind::Enum_decl )
        {
            continue;
        }

        const std::string_view name = interner_.text( Symbol_id { ast_.aux( child ) } );

        bounds_.declare_type_parameters( child, ast_.type_param_list( child ) );

        std::vector<Type_id> arguments;

        for( const Node_id type_param : type_parameters( ast_, ast_.type_param_list( child ) ) )
        {
            arguments.push_back( types_.type_of( type_param ) );
        }

        // Child 1 is the underlying type, invalid when unwritten. i32 by default, which is what
        // C++ gives a plain enum - D30 changed the semantics of the keyword, not its arithmetic.
        const Node_id annotation = ast_.child( child, 1 );
        Type_id       underlying = annotation.is_valid() ? annotations_.type_of( annotation ) : table_.integer( 32, true );

        // Only an integer can count variants. Absorbed to i32 so the rest of the pass has a type to
        // record; nothing downstream sees it, because a program with an error never reaches the emitter.
        if( !table_.is_integer( underlying ) && !table_.is_error( underlying ) )
        {
            reporter_.error_at(
                ast_.span( annotation ),
                fmt::format( "an `enum` must be backed by an integer, but `{}` is not one", table_.name( underlying ) )
            );

            underlying = table_.integer( 32, true );
        }

        const Type_id type = table_.enumeration( child, arguments, name, underlying );

        types_.record( child, type );

        // Reported per repeat rather than once, the same shape check_aggregate_members uses for a
        // field: three collisions should say so in one compile rather than over three.
        std::unordered_set<u32> seen;

        for( const Node_id variant : ast_.variants( child ) )
        {
            const Symbol_id variant_name { ast_.aux( variant ) };

            if( !variant_name.is_valid() )
            {
                continue;
            }

            if( !seen.insert( variant_name.v ).second )
            {
                reporter_.error_at(
                    ast_.span( variant ), fmt::format( "`{}` already has a variant `{}`", name, interner_.text( variant_name ) )
                );
            }
        }

        // The *enum* type, never the underlying one. D30's "no implicit conversion to an integer"
        // is enforced by this line and by the absence of a conversion rule for Enum anywhere else:
        // if a variant typed as i32, `i32 x = Colour::Red;` would compile and the entry would be a
        // comment rather than a rule.
        for( const Node_id variant : ast_.variants( child ) )
        {
            const Symbol_id variant_name { ast_.aux( variant ) };

            types_.record( variant, type );

            // D7: a variant's children are its payload fields, and they are Field_decls - so this
            // is the same recording declare_fields does for a struct.
            std::unordered_set<u32> fields;

            for( const Node_id field : ast_.children( variant ) )
            {
                const Type_id field_type = annotations_.type_of( ast_.child( field, 0 ) );

                types_.record( field, field_type );

                if( is_generic( ast_, child ) )
                {
                    generic_recursion_.record_generic_uses( child, field_type, ast_.span( field ) );
                }

                const Symbol_id field_name { ast_.aux( field ) };

                if( !fields.insert( field_name.v ).second && variant_name.is_valid() )
                {
                    reporter_.error_at(
                        ast_.span( field ),
                        fmt::format(
                            "`{}` already has a field `{}`", interner_.text( variant_name ), interner_.text( field_name )
                        )
                    );
                }
            }
        }
    }
}

void Signatures::order_structs()
{
    // D42 stays quiet about an aggregate on a containment cycle: having no size is the more basic
    // half of one mistake.
    for( const Node_id node : aggregates_.order_structs() )
    {
        generic_recursion_.mark_reported( node );
    }
}

void Signatures::check_aggregate_members()
{
    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        if( !is_aggregate( ast_.kind( decl ) ) )
        {
            continue;
        }

        check_aggregate_has_fields( decl );

        for( const Member_kind& kind : k_member_kinds )
        {
            check_member_kind( decl, kind );
        }
    }
}

void Signatures::check_member_kind( Node_id decl, const Member_kind& kind )
{
    const bool             on_a_struct = ast_.kind( decl ) == Node_kind::Struct_decl;
    const Symbol_id        type_name { ast_.aux( decl ) };
    const std::string_view type_text = interner_.text( type_name );

    Node_id first {};

    for( const Node_id member : ast_.members( decl ) )
    {
        if( ast_.kind( member ) != kind.node )
        {
            continue;
        }

        // One mistake, one diagnostic: declaring one of these on a struct is a single decision to
        // reverse, so the name and duplicate rules stay quiet about it.
        if( on_a_struct )
        {
            reporter_.error_at(
                ast_.span( member ), fmt::format( "a struct cannot have a {}", kind.noun ), std::string( kind.remedy )
            );
            return;
        }

        if( first.is_valid() )
        {
            // A destructor takes no parameters, so a second one has nothing to tell it apart. A
            // constructor does, and Overloads::check_overload_sets asks whether they differ.
            if( kind.only_one )
            {
                reporter_.error_at(
                    ast_.span( member ),
                    fmt::format( "`{}` already has a {}", type_text, kind.noun ),
                    reporter_.previous_declaration_note( ast_.span( first ) )
                );
            }

            continue;
        }

        first = member;

        const Symbol_id written { ast_.aux( member ) };

        if( written.is_valid() && written != type_name )
        {
            reporter_.error_at(
                ast_.span( member ),
                fmt::format( "`{}{}` does not name the enclosing type", kind.prefix, interner_.text( written ) ),
                fmt::format( "write `{}{}`", kind.prefix, type_text )
            );
        }
    }
}

void Signatures::check_aggregate_has_fields( Node_id decl )
{
    assert( is_aggregate( ast_.kind( decl ) ) );

    bool has_field = false;
    for( const Node_id member : ast_.members( decl ) )
    {
        if( ast_.kind( member ) == Node_kind::Field_decl )
        {
            has_field = true;
            break;
        }
    }

    if( !has_field )
    {
        reporter_.error_at(
            ast_.span( decl ),
            fmt::format( "`{}` has no fields, so it has no size", interner_.text( Symbol_id { ast_.aux( decl ) } ) ),
            "use an `enum` with one variant for a type with one value"
        );
    }
}

// D30's first cut. Destroying an enum means destroying only the *active* variant, chosen at run
// time - which needs a destructor that switches on the tag, and is the first run-time-dependent
// destructor the language would have. Until that exists an owning payload would leak on every path
// that did not construct it, so it is refused here rather than mis-destroyed there.
//
// Its own pass because is_owning_type only answers after compute_owning, which runs after the pass
// that declares enums - the same ordering that made record_borrowed_parameters a pass of its own.
void Signatures::check_enum_payloads()
{
    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( decl ) != Node_kind::Enum_decl )
        {
            continue;
        }

        for( const Node_id variant : ast_.variants( decl ) )
        {
            for( const Node_id field : ast_.children( variant ) )
            {
                if( bounds_.satisfies( types_.type_of( field ), Bound::Copyable ) )
                {
                    continue;
                }

                reporter_.error_at(
                    ast_.span( field ),
                    fmt::format( "a variant cannot carry `{}`, which owns a resource", table_.name( types_.type_of( field ) ) ),
                    "destroying an enum means destroying only the active variant, which is not implemented yet"
                );
            }
        }
    }
}

void Signatures::check_struct_ownership()
{
    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        // A struct with a destructor of its own is already reported by check_aggregate_members,
        // and it is one decision to reverse rather than two.
        if( ast_.kind( decl ) == Node_kind::Struct_decl && !keel::has_destructor( ast_, decl ) )
        {
            check_struct_fields_are_not_owning( decl );
        }
    }
}

void Signatures::check_struct_fields_are_not_owning( Node_id decl )
{
    for( const Node_id field : ast_.members( decl ) )
    {
        if( ast_.kind( field ) != Node_kind::Field_decl )
        {
            continue;
        }

        const Type_id field_type = types_.type_of( field );

        if( !field_type.is_valid() || table_.is_error( field_type ) )
        {
            continue;
        }

        // is_owning_type rather than the set directly: a field of type `T` has no declaration to
        // look up, and an unbounded one may turn out to own something - which is exactly what a
        // struct may not contain. The promise that rules it out is `Copyable`.
        if( bounds_.satisfies( field_type, Bound::Copyable ) )
        {
            continue;
        }

        const Node_id field_decl = table_.get( field_type ).declaration;

        // One per field: each is a separate place the author has to change.
        reporter_.error_at(
            ast_.span( field ),
            fmt::format(
                "a struct cannot contain `{}`, which {}",
                table_.name( field_type ),
                table_.is_parameter( field_type )          ? "may own a resource"
                : keel::has_destructor( ast_, field_decl ) ? "has a destructor"
                                                           : "owns a resource"
            ),
            table_.is_parameter( field_type )
                ? fmt::format(
                      "a struct is copied freely, so promise it can be: `where {} : Copyable`", table_.name( field_type )
                  )
                : fmt::format(
                      "a struct is copied freely, so declare `{}` as a class if it owns this",
                      interner_.text( Symbol_id { ast_.aux( decl ) } )
                  )
        );
    }
}

} // namespace sema
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

// C's main returns int and the emitted shim calls it with no arguments, so anything else either
// truncates silently or generates C that will not compile - a cc error pointing at generated code
// rather than a diagnostic pointing at the program.
TEST_CASE( "type_checker_constrains_the_signature_of_main", "[sema][types]" )
{
    SECTION( "i32 and no parameters is the one accepted form" )
    {
        const Typed p( "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "another integer type is rejected" )
    {
        const Typed p( "u64 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`main` must return `i32`" ) != std::string::npos );
    }

    SECTION( "so is void" )
    {
        const Typed p( "void main() { }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "parameters are rejected too - the shim passes none" )
    {
        const Typed p( "i32 main( i32 n ) { return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "parameters" ) != std::string::npos );
    }

    SECTION( "both wrong reports both" )
    {
        const Typed p( "u8 main( i32 n ) { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }

    // The unknown type is reported by Annotations::type_of; saying `main` must return i32 on top of
    // that would be two messages for one mistake.
    SECTION( "an unresolved return type reports once, as the unknown type" )
    {
        const Typed p( "Widget main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "unknown type" ) != std::string::npos );
    }

    SECTION( "the rule is about main alone" )
    {
        const Typed p( "u64 helper( i32 a, i32 b ) { return 0; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A program without one is a library. The emitter writes no shim, and nothing here objects.
    SECTION( "no main at all is not an error" )
    {
        const Typed p( "u64 helper() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_types_struct_declarations", "[sema][types]" )
{
    SECTION( "a struct is a type, and a field carries its own" )
    {
        const Typed p( "struct Point { f64 x; u32 n; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Struct_decl, 0 ) ) == "Point" );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 0 ) ) == "f64" );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 1 ) ) == "u32" );
    }

    // The field pass has to reach into each struct: fields are children of the Struct_decl, not of
    // the root, so a loop over the root's children finds none of them and a bad field type becomes
    // invisible.
    SECTION( "an unknown field type is reported" )
    {
        const Typed p( "struct Point { Widget w; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "unknown type `Widget`" ) != std::string::npos );
    }

    SECTION( "D1 applies to a field type too" )
    {
        const Typed p( "struct Point { double x; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "f64" ) != std::string::npos );
    }

    // Struct types must all exist before any field type resolves: `Line` names `Point`, which is
    // declared below it.
    SECTION( "a field may name a struct declared later" )
    {
        const Typed p( "struct Line { Point a; Point b; };\nstruct Point { f64 x; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a struct is usable in a signature" )
    {
        const Typed p( "struct Point { f64 x; };\nPoint make( Point p ) { return p; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The resolver binds the name; if it bound to something that is not a struct, that is not a
    // type however good the spelling looks.
    SECTION( "a function name is not a type" )
    {
        const Typed p( "i32 Point() { return 0; }\ni32 f( Point p ) { return 0; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// A struct containing itself by value has infinite size. C's error for this is incomprehensible,
// and the emitter's topological sort would not terminate.
TEST_CASE( "type_checker_rejects_recursive_structs", "[sema][types]" )
{
    SECTION( "directly" )
    {
        const Typed p( "struct Node { Node next; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    // Exactly one message: every struct on the cycle is marked when it is found, or the same cycle
    // is rediscovered from B and reported twice for one mistake.
    SECTION( "and through another struct, reported once" )
    {
        const Typed p( "struct A { B b; };\nstruct B { A a; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a three-struct cycle is also one message" )
    {
        const Typed p( "struct A { B b; };\nstruct B { C c; };\nstruct C { A a; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Containment is what makes it infinite. A chain that does not close is fine however deep.
    SECTION( "a deep chain that does not close is fine" )
    {
        const Typed p( "struct A { f64 x; };\nstruct B { A a; };\nstruct C { B b; };\n"
                       "struct D { C c; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a struct used twice is not a cycle" )
    {
        const Typed p( "struct Point { f64 x; };\nstruct Line { Point a; Point b; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// `main` is the one name the backend also generates a shim for, so an extern one would have the
// shim call the symbol it is standing in for - a silent infinite recursion rather than a link error.
TEST_CASE( "type_checker_rejects_an_extern_main", "[sema][types][extern]" )
{
    const Typed p( "extern i32 main();" );

    INFO( p.rendered() );
    REQUIRE_FALSE( p.clean() );
    REQUIRE( p.rendered().find( "`extern`" ) != std::string::npos );
}

// The predicate three passes share. Asked of the wrong kind it must answer false rather than
// assert: lower() puts every function-like node through it, destructors and constructors included.
TEST_CASE( "type_checker_is_extern_reads_the_absent_body", "[sema][types][extern]" )
{
    const Typed p( "extern i32 abs( i32 v );\n"
                   "class C { i32 x; C( i32 v ) { x = v; } ~C() { } i32 get() { return x; } };\n"
                   "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    REQUIRE( is_extern( p.ast(), p.nth( Node_kind::Function_decl, 0 ) ) );       // the extern
    REQUIRE_FALSE( is_extern( p.ast(), p.nth( Node_kind::Function_decl, 1 ) ) ); // main
    REQUIRE_FALSE( is_extern( p.ast(), p.nth( Node_kind::Constructor_decl, 0 ) ) );
    REQUIRE_FALSE( is_extern( p.ast(), p.nth( Node_kind::Destructor_decl, 0 ) ) );
    REQUIRE_FALSE( is_extern( p.ast(), p.nth( Node_kind::Method_decl, 0 ) ) );
    REQUIRE_FALSE( is_extern( p.ast(), p.nth( Node_kind::Class_decl, 0 ) ) );
}

// D29: a class is built by a constructor and a struct from a literal, which is what stops the two
// initialisation syntaxes competing. The parser accepts one on either kind so the message can name
// the fix rather than being a syntax error.
TEST_CASE( "type_checker_checks_constructor_declarations", "[sema][aggregates]" )
{
    SECTION( "a struct with a constructor is rejected" )
    {
        const Typed p( "struct Point { i32 x; Point( i32 a ) { x = a; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a class with one is accepted" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a name that does not match the enclosing type is rejected" )
    {
        const Typed p( "class Buffer { u64 len; Wrong( u64 n ) { len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    // One mistake, one diagnostic: declaring a constructor on a struct is a single decision to
    // reverse, so a second one says nothing new.
    SECTION( "a struct with two constructors is still one mistake" )
    {
        const Typed p( "struct Point { i32 x; Point( i32 a ) { x = a; } Point( u64 b ) { x = 1; } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "two constructors that differ coexist" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { } Buffer( i32 m ) { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "two constructors with the same parameters are rejected once" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { } Buffer( u64 m ) { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "already declared with these parameters" ) != std::string::npos );
    }

    SECTION( "a constructor beside a destructor is fine" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the body is type checked" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { len = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a constructor returns nothing" )
    {
        const Typed p( "class Buffer { u64 len; Buffer( u64 n ) { return 1; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// A field is not a binding, so `const` has nothing to attach to under this model. A write-once
// field is a feature waiting on definite assignment over a constructor body - the same analysis
// `out` needs - so the message says "yet" rather than pretending it is a rule of the language.
TEST_CASE( "type_checker_refuses_const_on_a_field_for_now", "[sema][const]" )
{
    const Typed p( "struct P { const i32 x; };\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "a field cannot be `const` yet" ) != std::string::npos );
}

// Assigning an `out` parameter destroys nothing, so an owning one would leak whatever the caller
// was already holding. Refused until the caller emits a drop before the call.
TEST_CASE( "type_checker_refuses_an_owning_out_parameter", "[sema][out]" )
{
    const Typed p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                   "void init( out B b ) { }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE_FALSE( p.clean() );
    REQUIRE( p.rendered().find( "owns a resource" ) != std::string::npos );
}

// PLAN D30. An enum is a type of its own, and a variant has that type - never the underlying
// integer. That one recording is what makes every rejection below fall out of rules that already
// existed rather than needing new ones.
TEST_CASE( "type_checker_types_an_enum_and_its_variants", "[sema][enum]" )
{
    const Typed p( "enum Colour : u8 { Red, Green, Blue };\ni32 main() { Colour c = Colour::Green; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.type_name( p.nth( Node_kind::Enum_decl, 0 ) ) == "Colour" );
    REQUIRE( p.type_name( p.nth( Node_kind::Variant_decl, 0 ) ) == "Colour" );
    REQUIRE( p.type_name( p.nth( Node_kind::Path_expr, 0 ) ) == "Colour" );
}

TEST_CASE( "type_checker_checks_an_enum_declaration", "[sema][enum]" )
{
    SECTION( "the underlying type defaults to i32" )
    {
        const Typed p( "enum Colour { Red };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and must be an integer when written" )
    {
        const Typed p( "enum Colour : bool { Red };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must be backed by an integer" ) != std::string::npos );
    }

    SECTION( "duplicate variants are refused, once each" )
    {
        const Typed p( "enum Colour { Red, Green, Red, Red };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "already has a variant `Red`" ) != std::string::npos );
    }
}

// Slice 1a of M6: the shape parses and the resolver scopes the parameters, and sema says once that
// nothing further is built yet. Reporting once matters - typing the body would report an unknown
// type for every use of `T`, turning one unimplemented feature into five errors.
// A generic's signature is typed with `T` standing for itself, so the parameters resolve like any
// other named type and nothing in the signature reports as unknown. The body is not checked yet -
// that is D11's definition-checking, and until it lands an instance is checked at its expansion.
// The order of the eleven steps in the declaration pass is this class's own fact. This is the
// edge nothing else rests on: an enum is a type that a field, a parameter, a return type or a
// global may name, so it is declared before any of them resolves. Out of order the annotation
// silently resolves to nothing - no diagnostic at all, and the crash lands in codegen.
TEST_CASE( "type_checker_declares_an_enum_before_anything_that_can_name_one", "[sema][enum][types]" )
{
    SECTION( "a field" )
    {
        const Typed p( "enum Colour { Red, Green };\n"
                       "struct Holder { Colour c; };\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_decl, 0 ) ) == "Colour" );
    }

    SECTION( "a method's return type" )
    {
        const Typed p( "enum Colour { Red, Green };\n"
                       "class Box { i32 v; Colour tint() { return Colour::Red; } };\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Method_decl, 0 ) ) == "Colour" );
    }

    SECTION( "a method's parameter" )
    {
        const Typed p( "enum Colour { Red, Green };\n"
                       "class Box { i32 v; void paint( Colour c ) { v = 1; } };\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Param_decl, 1 ) ) == "Colour" );
    }

    SECTION( "a global" )
    {
        // Uninitialised: a variant is not a constant expression, which is a different rule.
        const Typed p( "enum Colour { Red, Green };\n"
                       "Colour background;\n"
                       "i32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Var_decl, 0 ) ) == "Colour" );
    }
}

TEST_CASE( "type_checker_types_a_generic_signature", "[sema][generic]" )
{
    SECTION( "the declaration alone is accepted" )
    {
        const Typed p( "T id<T>( T a ) where T : Copyable { return a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the type parameters are not unknown types" )
    {
        const Typed p( "T twice<T>( T a, T b ) { T c = a; return c; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "unknown type" ) == std::string::npos );
    }

    SECTION( "a broken non-generic beside it is still checked" )
    {
        const Typed p( "T id<T>( T a ) where T : Copyable { return a; }\n"
                       "i32 broken() { return true; }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and a plain program is untouched" )
    {
        const Typed p( "i32 f( i32 a ) { return a; }\ni32 main() { return f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D26 meeting a type parameter. A literal has a value rather than a type, and context supplies one -
// but a bound supplies a *set* of types, so the question becomes whether the value fits every type
// `T` may turn out to be. Answering it here rather than at an instantiation is what keeps D11 whole:
// no later call site can break a body that already compiled.
// Monomorphisation emits one function per set of type arguments, so it terminates only if that set
// is finite. Forwarding a parameter keeps it finite however deep the recursion runs; building a type
// out of it does not, and the shape is decidable at the definition rather than at whichever
// expansion happens to hit a limit.
// A type parameter or a `where` subject whose name failed to parse must not become a node. The
// parser reports either way; what a nameless node costs is sema, which reads that name to report
// about it and hands an invalid one to the interner - which asserts, so the compiler dies on a file
// it had already diagnosed. Nine spellings, two nodes, and the same reading parse_aggregate_decl
// takes of a declaration with no name.
TEST_CASE( "type_checker_survives_a_nameless_type_parameter_or_clause", "[sema][generic]" )
{
    const auto diagnosed = []( std::string_view source )
    {
        const Typed p( std::string( source ) + "\ni32 main() { return 0; }" );

        // Reaching here at all is the assertion: the failure mode is an abort, not a wrong answer.
        return !p.clean();
    };

    SECTION( "a malformed type parameter list" )
    {
        REQUIRE( diagnosed( "T f<T,>( T a ) { return a; }" ) );
        REQUIRE( diagnosed( "T f<,T>( T a ) { return a; }" ) );
        REQUIRE( diagnosed( "T f<T,,U>( T a ) { return a; }" ) );
        REQUIRE( diagnosed( "T f<if>( T a ) { return a; }" ) );
    }

    SECTION( "a `where` clause with no subject" )
    {
        REQUIRE( diagnosed( "T f<T>( T a ) where { return a; }" ) );
        REQUIRE( diagnosed( "T f<T>( T a ) where : Copyable { return a; }" ) );
        REQUIRE( diagnosed( "T f<T>( T a ) where where T : Copyable { return a; }" ) );
        REQUIRE( diagnosed( "T f<T>( T a ) where 5 : Copyable { return a; }" ) );
        REQUIRE( diagnosed( "T f<T>( T a ) where if : Copyable { return a; }" ) );
    }

    SECTION( "a well-formed one still works" )
    {
        // The guard drops the node rather than the feature.
        const Typed p( "T f<T>( T a ) where T : Copyable { return a; }\ni32 main() { return f<i32>( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Self-containment is about size, and size is about by-value members - so the existing walk answers
// it for a generic aggregate unchanged, including the shape that grows a type each time round.
TEST_CASE( "type_checker_refuses_a_generic_aggregate_that_contains_itself", "[sema][generic][aggregate]" )
{
    SECTION( "directly" )
    {
        const Typed p( "struct Odd<T> where T : Copyable { Odd<T> inner; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`Odd` contains itself" ) != std::string::npos );
    }

    SECTION( "through a type built from its own parameter" )
    {
        // Infinite twice over: no size, and no finite set of instantiations either. The size rule
        // catches it first, which is the same answer for a stronger reason.
        const Typed p( "struct Box<T> where T : Copyable { T v; };\n"
                       "struct Odd<T> where T : Copyable { Odd<Box<T>> inner; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`Odd` contains itself" ) != std::string::npos );

        // And only that one: two messages for one mistake sends the author round twice.
        REQUIRE( p.rendered().find( "bigger type argument" ) == std::string::npos );
    }

    SECTION( "a pointer to itself is finite, and is how a list is written" )
    {
        const Typed p( "class Node<T> where T : Copyable { Node<T>* next; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "holding a different instantiation is ordinary containment" )
    {
        // A generic holding a *concrete* instantiation of another is a finite thing, unlike the
        // case above: `Holder<i32>` needs `Box<i32>` and `Box<i32>` needs nothing back.
        const Typed p( "struct Box<T> where T : Copyable { T v; };\n"
                       "struct Holder<T> where T : Copyable { Box<i32> b; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_scopes_type_parameters", "[sema][generic]" )
{
    SECTION( "a duplicate is reported" )
    {
        const Typed p( "T id<T, T>( T a ) { return a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "already declared" ) != std::string::npos );
    }

    SECTION( "they do not escape the declaration" )
    {
        const Typed p( "T id<T>( T a ) where T : Copyable { return a; }\nT stray( T a ) { return a; }\ni32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "unknown type `T`" ) != std::string::npos );
    }

    SECTION( "two declarations may reuse the same parameter name" )
    {
        const Typed p( "T one<T>( T a ) where T : Copyable { return a; }\n"
                       "T two<T>( T a ) where T : Copyable { return a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );

        // Each declaration's `T` is its own type, keyed by its own Type_param_decl.
        REQUIRE( p.clean() );
    }
}

// PLAN §12. An aggregate with no fields emits a C struct with no members, which standard C has no
// spelling for, and it is the only route left to a zero-size `alloc`. The predicate is fields
// rather than members: a method-only class is the same empty struct once emitted.
TEST_CASE( "type_checker_rejects_empty_aggregates", "[sema][types]" )
{
    SECTION( "a struct with no fields is refused" )
    {
        const Typed p( "struct Empty { };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Empty` has no fields" ) != std::string::npos );
    }

    SECTION( "a class with no fields is refused" )
    {
        const Typed p( "class Empty { };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Empty` has no fields" ) != std::string::npos );
    }

    // The case a "members list is empty" predicate misses: a method takes no storage, so this is
    // the same empty C struct as one written with nothing in it.
    SECTION( "a method does not give a class a field" )
    {
        const Typed p( "class Marker { i32 get() { return 1; } };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Marker` has no fields" ) != std::string::npos );
    }

    // Nor does a lifecycle member. One diagnostic, not two: there is nothing here for the
    // ownership rules to object to, since the type holds nothing to own.
    SECTION( "a constructor and destructor do not give a class a field" )
    {
        const Typed p( "class Guard { Guard() { } ~Guard() { } };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Guard` has no fields" ) != std::string::npos );
    }

    // Empty for every `T`, so it is caught where it is written rather than once per instantiation -
    // which is also why an uninstantiated generic still has to report it.
    SECTION( "a generic aggregate is refused at its declaration" )
    {
        const Typed p( "struct Box<T> { };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Box` has no fields" ) != std::string::npos );
    }

    // The alternative the diagnostic names. Conflating "no fields" with "no data" would reject the
    // fix the message recommends, so this is the section that pins the rule's edge.
    SECTION( "an enum with one variant is accepted" )
    {
        const Typed p( "enum Unit { Only };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a payload-free variant is accepted beside one that carries data" )
    {
        const Typed p( "enum Shape { Nothing, Circle( f64 radius ) };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "one field is enough" )
    {
        const Typed p( "struct One { i32 x; };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D29 draws the struct/class line at trivial copyability, and a destructor is what breaks it: copy
// plus destructor is a double free. The parser accepts one on either kind so that the message can
// name `class` as the fix rather than being a syntax error.
TEST_CASE( "type_checker_rejects_a_destructor_on_a_struct", "[sema][aggregates]" )
{
    SECTION( "a struct with a destructor is rejected" )
    {
        const Typed p( "struct Point { i32 x; ~Point() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "cannot have a destructor" ) != std::string::npos );
    }

    // One mistake, one diagnostic - the rest of the declaration is well formed and must not be
    // re-reported as a consequence of the destructor.
    SECTION( "and reported once" )
    {
        const Typed p( "struct Point { i32 x; i32 y; ~Point() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a class with one is accepted" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a class without one is accepted" )
    {
        const Typed p( "class Handle { u64 value; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// aux carries what was written after the `~`, so a mismatch is a comparison rather than a parse
// failure - which is what lets the message name the type that was meant.
TEST_CASE( "type_checker_checks_destructor_names", "[sema][aggregates]" )
{
    SECTION( "a name that does not match the enclosing type is rejected" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Wrong() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a matching name is accepted" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The resolver's duplicate-member check skips everything that is not a Field_decl, so a second
    // destructor reaches here unreported and needs its own rule.
    SECTION( "two destructors are rejected once" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }
}

// D2: a type is owning exactly when it has a destructor, directly or through a by-value member.
// The answer is recorded rather than recomputed because drop elaboration runs on KIR, long after
// the checker has finished.
TEST_CASE( "type_checker_computes_the_owning_query", "[sema][aggregates][owning]" )
{
    const auto owning = []( const Typed& p, std::size_t nth_decl )
    { return p.types().is_owning( p.types().type_of( p.nth( Node_kind::Class_decl, nth_decl ) ) ); };

    SECTION( "a class with a destructor owns; one without does not" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "class Handle { u64 value; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( owning( p, 0 ) );
        REQUIRE_FALSE( owning( p, 1 ) );
    }

    SECTION( "a struct never owns - it cannot hold anything that does" )
    {
        const Typed p( "struct Point { i32 x; i32 y; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE_FALSE( p.types().is_owning( p.types().type_of( p.nth( Node_kind::Struct_decl, 0 ) ) ) );
    }

    // The transitivity is forced rather than chosen: destroying a Wrapper destroys its Buffer, so
    // there is no way for it not to own.
    SECTION( "owning is transitive through a by-value member" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "class Wrapper { Buffer inner; };\n"
                       "class Outer { Wrapper w; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( owning( p, 1 ) );
        REQUIRE( owning( p, 2 ) );
    }

    // An address says nothing about who frees it, so a pointer breaks the chain.
    SECTION( "a pointer to an owning type does not own" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "class Holder { Buffer* p; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE_FALSE( owning( p, 1 ) );
    }

    SECTION( "a builtin never owns" )
    {
        const Typed p( "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.types().is_owning( p.types().table().integer( 32, true ) ) );
    }
}

// The rule that makes `struct` mean something: it is legal exactly when D2's owning query says no,
// so the check costs one call rather than any new machinery.
TEST_CASE( "type_checker_rejects_an_owning_member_in_a_struct", "[sema][aggregates][owning]" )
{
    SECTION( "a struct holding a class with a destructor is rejected" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer b; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "transitively, through a class that only contains one" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "class Wrapper { Buffer inner; };\n"
                       "struct Holder { Wrapper w; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a pointer to one is fine - it owns nothing" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer* p; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A destructor is a child of the declaration alongside the fields, so anything counting
    // children counts it too - which demanded an extra initialiser and misaligned every positional
    // one after it. The literal form on a class is D29's temporary exception until constructors
    // exist, so it is exactly the path with no other coverage.
    SECTION( "a destructor is not counted as a field by a struct literal" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "i32 main() { Buffer b = Buffer { nullptr }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a genuinely wrong count still reports the field count, not the member count" )
    {
        const Typed p( "class Buffer { u8* ptr; u64 len; ~Buffer() { } };\n"
                       "i32 main() { Buffer b = Buffer { nullptr }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "has 2 fields" ) != std::string::npos );
    }

    SECTION( "a class holding one is fine" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "class Wrapper { Buffer inner; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a struct holding a non-owning class is fine" )
    {
        const Typed p( "class Handle { u64 value; };\n"
                       "struct Holder { Handle h; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A struct that declares a destructor is already reported for that, and its owning fields
    // are the same decision to reverse rather than a second one.
    SECTION( "a struct with a destructor of its own is reported once" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer b; ~Holder() { } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Every field is reported, because each is a separate place the author has to change.
    SECTION( "two owning fields are two diagnostics" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { } };\n"
                       "struct Holder { Buffer a; Buffer b; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }
}

// The destructor body is typed like any function body. `this` needs no rule of its own: it is a
// parameter with an annotation, so the ordinary path types it.
TEST_CASE( "type_checker_types_a_destructor_body", "[sema][types][aggregates]" )
{
    SECTION( "a bare field has the field's type" )
    {
        const Typed p( "class Buffer { u8* ptr; u64 len; ~Buffer() { len = 0; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Name_expr, 0 ) ) == "u64" );
    }

    SECTION( "`this` is a reference to the enclosing type" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { this.ptr = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Name_expr, 0 ) ) == "Buffer" );
    }

    // D22 already makes `.` reach through a pointer, so `this.ptr` needs no rule either.
    SECTION( "`this.field` has the field's type" )
    {
        const Typed p( "class Buffer { u64 len; ~Buffer() { this.len = 0; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Field_expr, 0 ) ) == "u64" );
    }

    SECTION( "a field's type is still checked against what is assigned" )
    {
        const Typed p( "class Buffer { u64 len; ~Buffer() { len = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // C++ forbids it, and a pointer parameter would otherwise accept it silently.
    SECTION( "`this` cannot be assigned" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { this = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a destructor returns nothing" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { return 1; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a bare `return` is fine" )
    {
        const Typed p( "class Buffer { u8* ptr; ~Buffer() { return; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "calling a free function from a destructor works" )
    {
        const Typed p( "void release( u8* p ) { }\n"
                       "class Buffer { u8* ptr; ~Buffer() { release( ptr ); } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D2 counts only by-value containment, so what a generic aggregate owns is a question about the
// *field's* type rather than about the declaration - and an unbounded `T` may turn out to own
// something. Nothing new was needed for any of this; it falls out of rules already written.
TEST_CASE( "type_checker_applies_the_owning_rules_to_a_generic_aggregate", "[sema][generic][aggregate]" )
{
    SECTION( "a struct may not hold a `T` that might own something" )
    {
        const Typed p( "struct Pair<T> { T a; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "a struct cannot contain `T`, which may own a resource" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`where T : Copyable`" ) != std::string::npos );
    }

    SECTION( "the promise that rules it out is `Copyable`" )
    {
        const Typed p( "struct Pair<T> where T : Copyable { T a; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a class may hold one, because a class is not copied" )
    {
        const Typed p( "class Box<T> { T a; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a pointer to one owns nothing, whatever `T` is" )
    {
        // Only by-value containment counts, which is the existing rule and needed no generic case.
        const Typed p( "struct Ref<T> { T* a; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Ownership is a question about the *instantiation*, not the declaration: `Box<i32>` and
// `Box<Buf>` come from one class and one of them owns. Types::is_owning still answers from the
// declaration, which is the open form's answer and what the checker's move rules want inside a
// generic body - instance_owns is the other half, and the one a drop is elaborated from.
TEST_CASE( "type_checker_answers_ownership_per_instantiation", "[sema][generic][aggregate]" )
{
    const std::string_view owner = "class Buf\n"
                                   "{\n"
                                   "    i32* p;\n"
                                   "    Buf() { unsafe { p = alloc<i32>(); } }\n"
                                   "    ~Buf() { unsafe { free( p ); } }\n"
                                   "};\n"
                                   "class Box<T> { T v; };\n";

    SECTION( "an argument that owns something makes the instance own" )
    {
        Typed p( std::string( owner ) + "i32 main() { Box<Buf> b; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const Type_id instance = p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE( instance_owns( p.ast(), p.types().table(), instance, p.types().recorded() ) );
    }

    SECTION( "the same declaration at an argument that does not" )
    {
        Typed p( std::string( owner ) + "i32 main() { Box<i32> b; b.v = 1; return b.v; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const Type_id instance = p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE_FALSE( instance_owns( p.ast(), p.types().table(), instance, p.types().recorded() ) );
        // The declaration's answer is the one that cannot tell them apart, which is why both
        // questions exist.
        REQUIRE_FALSE( p.types().is_owning( instance ) );
    }

    SECTION( "a pointer to an owning type owns nothing" )
    {
        // D2 counts only by-value containment, here as everywhere else.
        Typed p(
            std::string( owner ) + "class Ref<T> { T* v; };\n"
                                   "i32 main() { Ref<Buf> r; return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const Type_id instance = p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE_FALSE( instance_owns( p.ast(), p.types().table(), instance, p.types().recorded() ) );
    }

    SECTION( "a destructor of its own makes every instance own" )
    {
        Typed p( "class Held<T> { T v; ~Held() { } };\n"
                 "i32 main() { Held<i32> h; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const Type_id instance = p.types().type_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE( instance_owns( p.ast(), p.types().table(), instance, p.types().recorded() ) );
    }
}

// D30's first cut. Destroying an enum means destroying only the *active* variant, which needs a
// destructor that switches on the tag - the first run-time-dependent destructor in the language.
// Until that exists, an owning payload would leak or double-free, so it is refused.
TEST_CASE( "type_checker_refuses_an_owning_payload_for_now", "[sema][payload]" )
{
    const Typed p( "class B { u64 n; B( u64 x ) { n = x; } ~B() { } };\n"
                   "enum Holder { Full( B value ), Empty };\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE_FALSE( p.clean() );
    REQUIRE( p.rendered().find( "owns a resource" ) != std::string::npos );
}

} // namespace keel
#endif
