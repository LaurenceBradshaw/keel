// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/type_checker.h"
#include <fmt/format.h>
#include <algorithm>
#include "common/source_manager.h"
#include "sema/aggregates.h"
#include "sema/annotations.h"
#include "sema/bounds.h"
#include "sema/callees.h"
#include "sema/constant_folder.h"
#include "sema/coverage.h"
#include "sema/expressions.h"
#include "sema/generic_recursion.h"
#include "sema/literals.h"
#include "sema/operators.h"
#include "sema/overloads.h"
#include "sema/places.h"
#include "sema/reporter.h"
#include "sema/signatures.h"
#include "sema/statements.h"
#include "sema/types_builder.h"

// The entry point, the two passes it sequences, and the queries later passes ask of the result.
// `Checker` owns the fourteen rule classes and does nothing else: every rule lives in one of them,
// and each one may only name the classes constructed above it.
//
// The class is declared here rather than in a header because this is the only file that defines a
// member of it. `keel::sema` rather than `keel` because everything in it left an anonymous
// namespace to get here, and `Bound` and `Constant` are names a later module should not have to
// avoid. Nothing outside sema/ names `Checker`; the public surface is `type_check()` below.

namespace keel::sema
{

class Checker
{
public:
    Checker(
        const Ast&            ast,
        const Interner&       interner,
        const Resolution&     resolution,
        const Source_manager& source_manager,
        const Literal_pool&   literals,
        Diagnostics&          diags
    )
        : ast_( ast ),
          interner_( interner ),
          resolution_( resolution ),
          reporter_( source_manager, diags ),
          table_( types_.table() ),
          aggregates_( ast, interner, types_, reporter_ ),
          bounds_( ast, interner, literals, types_, aggregates_, reporter_ ),
          annotations_( ast, interner, resolution, types_, bounds_, reporter_ ),
          constant_folder_( ast, literals, types_, bounds_, reporter_ ),
          places_( ast, interner, resolution, types_, bounds_, callees_, reporter_ ),
          generic_recursion_( ast, interner, table_, reporter_ ),
          literals_( ast, literals, types_, bounds_, constant_folder_, reporter_ ),
          operators_( ast, interner, table_, bounds_, reporter_ ),
          overloads_( ast, interner, resolution, types_, aggregates_, bounds_, annotations_, reporter_ ),
          expressions_(
              ast,
              interner,
              resolution,
              types_,
              aggregates_,
              bounds_,
              annotations_,
              constant_folder_,
              callees_,
              places_,
              generic_recursion_,
              literals_,
              operators_,
              overloads_,
              reporter_
          ),
          coverage_( ast, interner, types_, aggregates_, constant_folder_, expressions_, reporter_ ),
          statements_(
              ast,
              interner,
              resolution,
              types_,
              bounds_,
              annotations_,
              constant_folder_,
              places_,
              expressions_,
              coverage_,
              reporter_
          ),
          signatures_(
              ast, interner, types_, aggregates_, bounds_, annotations_, places_, generic_recursion_, overloads_, reporter_
          )
    {
    }

    Types run();

private:
    const Ast&        ast_;
    const Interner&   interner_;
    const Resolution& resolution_;

    Reporter      reporter_;
    Types_builder types_;

    // A view into types_, bound once. The table has one owner and this is not it; the rule classes
    // below will hold the same reference for the same reason.
    Type_table& table_;

    Aggregates        aggregates_;
    Bounds            bounds_;
    Annotations       annotations_;
    Constant_folder   constant_folder_;
    Callees           callees_;
    Places            places_;
    Generic_recursion generic_recursion_;
    Literals          literals_;
    Operators         operators_;
    Overloads         overloads_;
    Expressions       expressions_;
    Coverage          coverage_;
    Statements        statements_;
    Signatures        signatures_;
};

Types Checker::run()
{
    types_.size_to( ast_.node_count() );

    for( const Node_id decl : ast_.declarations( ast_.root() ) )
    {
        const Node_kind kind    = ast_.kind( decl );
        const Symbol_id package = resolution_.package_of( ast_.span( decl ).file );

        if( ( is_aggregate( kind ) || kind == Node_kind::Enum_decl ) && package.is_valid() )
        {
            types_.table().set_package( decl, interner_.text( package ), package != resolution_.prelude_package() );
        }
    }

    signatures_.declare();
    statements_.visit( ast_.root() );

    // After the bodies: the graph it walks is built one edge per generic call typed, so it is not
    // complete until every body has been.
    generic_recursion_.check();

    return Types(
        types_.take_table(),
        types_.take_types(),
        aggregates_.take_struct_order(),
        constant_folder_.take_values(),
        callees_.take(),
        generic_recursion_.take_instantiations(),
        overloads_.take_instantiations(),
        generic_recursion_.take_generic_calls()
    );
}

} // namespace keel::sema

namespace keel
{

Types type_check(
    const Ast&            ast,
    const Resolution&     resolution,
    const Literal_pool&   literals,
    const Source_manager& sm,
    const Interner&       interner,
    Diagnostics&          diags
)
{
    return sema::Checker( ast, interner, resolution, sm, literals, diags ).run();
}

Param_mode parameter_mode_of( const Ast& ast, Node_id param )
{
    switch( ast.parameter_mode( param ) )
    {
    case Keyword::Ref:
        return ast.is_const_binding( param ) ? Param_mode::Const_ref : Param_mode::Ref;
    case Keyword::Out:
        return Param_mode::Out;
    case Keyword::Move:
        return Param_mode::Move;
    default:
        return Param_mode::Value;
    }
}

Keyword call_marker_of( Param_mode mode )
{
    switch( mode )
    {
    case Param_mode::Ref:
        return Keyword::Ref;
    case Param_mode::Out:
        return Keyword::Out;
    case Param_mode::Move:
        return Keyword::Move;
    // Bare and `const ref` both: after either, the caller's object is alive and unchanged, which is
    // the whole of what a marker announces.
    default:
        return Keyword::Count;
    }
}

bool is_borrowed_binding( const Ast& ast, const Types& types, Node_id decl )
{
    const Node_id annotation = ast.declared_type( decl );

    // `auto` has no annotation node at all, so guard before asking.
    return annotation.is_valid() && types.type_of( annotation ).is_valid();
}

// What binds an aggregate's type parameters to what it was instantiated at. Empty for a
// non-generic one, and the identity for the open form, so callers need no branch.
//
// The recorded types come in as a span rather than through a Types, because the checker asks this
// while it is still filling that vector and has no Types to hand.
Bindings aggregate_bindings( const Ast& ast, const Type_table& table, Type_id aggregate, std::span<const Type_id> recorded )
{
    if( !aggregate.is_valid() || ( !table.is_struct( aggregate ) && !table.is_enum( aggregate ) ) )
    {
        return {};
    }

    const std::span<const Type_id> arguments = table.get( aggregate ).arguments;

    const std::vector<Node_id> parameters = ast.type_parameters( ast.type_param_list( table.get( aggregate ).declaration ) );

    Bindings bindings;

    for( std::size_t i = 0; i < parameters.size() && i < arguments.size(); ++i )
    {
        bindings.emplace( recorded[parameters[i].v].v, arguments[i] );
    }

    return bindings;
}

// A field's type *as seen through this instance*. The declared type of `Box<T>`'s field is `T`,
// which is true of the template and of no value anyone holds - so every read of a field's type goes
// through here rather than through the recorded one.
//
// The table is mutable because substituting can intern: `Box<T>`'s field of type `Pair<T>` becomes
// `Pair<i32>`, which may be a type nothing has named before.
Type_id field_type( const Ast& ast, Type_table& table, Type_id aggregate, Node_id field, std::span<const Type_id> recorded )
{
    if( !field.is_valid() || field.v >= recorded.size() )
    {
        return Type_id {};
    }

    // An empty map makes this the identity, which is every non-generic aggregate - so the ordinary
    // path costs a lookup that finds nothing rather than a branch here.
    return table.substitute( recorded[field.v], aggregate_bindings( ast, table, aggregate, recorded ) );
}

std::vector<Node_id>
carried_payload( const Ast& ast, Type_table& table, Type_id instance, Node_id variant, std::span<const Type_id> recorded )
{
    const std::span<const Node_id> payload = ast.payload( variant );
    std::vector<Node_id>           carried;
    for( const Node_id field : payload )
    {
        if( !table.is_void( field_type( ast, table, instance, field, recorded ) ) )
        {
            carried.push_back( field );
        }
    }

    return carried;
}

// The recursion behind instance_owns. `visiting` is not a cycle *check* - order_structs already
// refuses a by-value cycle between declarations - but instances are interned as they are asked
// about, and answering the same one twice down a chain would not terminate.
bool may_own(
    const Ast&                            ast,
    Type_table&                           table,
    Type_id                               instance,
    std::span<const Type_id>              recorded,
    std::vector<Type_id>&                 visiting,
    const std::function<bool( Type_id )>& parameter_owns
)
{
    if( !instance.is_valid() || table.is_error( instance ) )
    {
        return false;
    }

    if( table.is_parameter( instance ) )
    {
        return parameter_owns( instance );
    }

    if( table.is_union( instance ) )
    {
        const std::span<const Type_id> members = table.get( instance ).arguments;

        for( const Type_id member : members )
        {
            if( may_own( ast, table, member, recorded, visiting, parameter_owns ) )
            {
                return true;
            }
        }

        return false;
    }

    if( !table.is_struct( instance ) && !table.is_enum( instance ) )
    {
        return false;
    }

    const Node_id declaration = table.get( instance ).declaration;

    if( !declaration.is_valid() )
    {
        return false;
    }

    if( ast.has_destructor( declaration ) )
    {
        return true;
    }

    if( std::find( visiting.begin(), visiting.end(), instance ) != visiting.end() )
    {
        return false;
    }

    visiting.push_back( instance );

    // Only by-value containment, exactly as D2 counts it everywhere else: an address says nothing
    // about who frees what it points at, and field_type is what applies this instance's bindings.
    for( const Node_id field : ast.contained_fields( declaration ) )
    {
        if( may_own( ast, table, field_type( ast, table, instance, field, recorded ), recorded, visiting, parameter_owns ) )
        {
            visiting.pop_back();
            return true;
        }
    }

    visiting.pop_back();
    return false;
}

bool instance_owns( const Ast& ast, Type_table& table, Type_id instance, std::span<const Type_id> recorded )
{
    std::vector<Type_id> visiting;

    return may_own( ast, table, instance, recorded, visiting, []( Type_id ) { return true; } );
}

Type_id binding_type( const Ast& ast, const Types& types, Node_id param )
{
    return is_borrowed_binding( ast, types, param ) ? types.type_of( ast.declared_type( param ) ) : types.type_of( param );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

// Only what `run()` itself owns. Every other fixture was attributed to the class that emits its
// diagnostic and moved there; these two belong to no class, because they are claims about the whole
// pass rather than about any rule in it.

namespace keel
{

TEST_CASE( "type_checker_accepts_a_well_typed_program", "[sema][types]" )
{
    const Typed p( "i32 add( i32 a, i32 b )\n"
                   "{\n"
                   "    i32 total = a + b;\n"
                   "    return total;\n"
                   "}\n"
                   "\n"
                   "i32 main()\n"
                   "{\n"
                   "    return add( 1, 2 );\n"
                   "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

// The error type must absorb, the way Node_kind::Error does in the parser: one unresolved name
// produces one diagnostic, not one per enclosing expression.
TEST_CASE( "type_checker_does_not_cascade_from_an_error", "[sema][types]" )
{
    SECTION( "an unknown name is the resolver's error alone" )
    {
        const Typed p( "i32 main() { return unknown + 1 * 2; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "an unknown type is reported once" )
    {
        const Typed p( "i32 main() { Widget w = 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Neither an arity complaint for `Map<i32>` nor a local named `i32` hiding the type.
    SECTION( "a broken type argument list is the parser's error alone" )
    {
        const Typed p( "class Map<K, V> { K k; V v; };\n"
                       "i32 main() { Map<i32 i32>* m; i32 b = 1; return b; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "and a parse error does not reach the checker at all" )
    {
        const Typed p( "i32 main() { return 1 +; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }
}

} // namespace keel
#endif
