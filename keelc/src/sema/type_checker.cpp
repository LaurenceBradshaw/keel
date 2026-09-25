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

namespace keel
{

namespace sema
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
    const Ast& ast_;

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
        aggregates_.take_owning(),
        callees_.take(),
        generic_recursion_.take_instantiations(),
        overloads_.take_instantiations(),
        generic_recursion_.take_generic_calls()
    );
}

} // namespace sema

Types type_check(
    const Ast&            ast,
    const Resolution&     resolution,
    const Literal_pool&   literals,
    const Source_manager& sm, // not needed yet; kept so the pass signatures match
    const Interner&       interner,
    Diagnostics&          diags
)
{
    return sema::Checker( ast, interner, resolution, sm, literals, diags ).run();
}

bool enum_has_payload( const Ast& ast, Node_id enum_decl )
{
    for( const Node_id variant : ast.variants( enum_decl ) )
    {
        if( !ast.children( variant ).empty() )
        {
            return true;
        }
    }

    return false;
}

Keyword parameter_mode( const Ast& ast, Node_id decl )
{
    const Node_id annotation = unwrap_const( ast, ast.child( decl, 0 ) );

    return annotation.is_valid() && ast.kind( annotation ) == Node_kind::Mode_type
               ? static_cast<Keyword>( ast.aux( annotation ) )
               : Keyword::Count;
}

bool is_ref_parameter( const Ast& ast, Node_id param )
{
    // children[0] is invalid for an `auto` local, and for a constructor or destructor, which have
    // no return type to carry a mode. kind() asserts on an invalid id rather than answering.
    const Node_id annotation = unwrap_const( ast, ast.child( param, 0 ) );

    return annotation.is_valid() && ast.kind( annotation ) == Node_kind::Mode_type &&
           static_cast<Keyword>( ast.aux( annotation ) ) == Keyword::Ref;
}

// The type parameters of a declaration, without the `where` clauses that share their list. Four
// places want exactly this and three of them were counting the clauses.
std::vector<Node_id> type_parameters( const Ast& ast, Node_id decl )
{
    // A non-generic declaration carries the slot with nothing in it, so "no list" is the ordinary
    // answer rather than a caller's mistake - and every caller would otherwise guard first.
    if( !decl.is_valid() )
    {
        return {};
    }

    assert( ast.kind( decl ) == Node_kind::Type_param_list );
    std::vector<Node_id> result;
    for( const Node_id param : ast.children( decl ) )
    {
        if( ast.kind( param ) != Node_kind::Type_param_decl )
        {
            continue;
        }

        result.push_back( param );
    }

    return result;
}

bool is_generic( const Ast& ast, Node_id decl )
{
    return ast.type_param_list( decl ).is_valid();
}

bool is_extern( const Ast& ast, Node_id decl )
{
    return ast.kind( decl ) == Node_kind::Function_decl && !ast.child( decl, 2 ).is_valid();
}

// Visibility is one comparison, and `from` is an aggregate declaration rather than the function or
// the receiver the access was written in: asking anything else forces every caller to convert, and
// the conversion is where the two halves stop meaning the same thing.
//
// An invalid `from` is a free function, which is outside every type and so sees nothing private.
bool is_visible_from( const Ast& ast, Node_id member, Node_id from )
{
    if( !member.is_valid() || ast.access( member ) != Access::Private )
    {
        return true;
    }

    return from.is_valid() && enclosing_aggregate( ast, member ) == from;
}

// Whether parameter 0 is the synthesised `this`. Asked of the parameter rather than of the node
// kind, because three kinds have a receiver and two do not, and M7 made the second group hold both
// a free function and a member. `this` is a keyword, so no written parameter can carry its name and
// the test is exact.
bool has_receiver( const Ast& ast, Node_id decl )
{
    if( !decl.is_valid() || !is_function_like( ast.kind( decl ) ) )
    {
        return false;
    }

    const std::span<const Node_id> params = ast.children( ast.child( decl, 1 ) );

    return !params.empty() && Symbol_id { ast.aux( params[0] ) } == Interner::keyword( Keyword::This );
}

bool is_static_method( const Ast& ast, Node_id method )
{
    return method.is_valid() && ast.kind( method ) == Node_kind::Method_decl && !has_receiver( ast, method );
}

bool is_borrowed_binding( const Ast& ast, const Types& types, Node_id decl )
{
    const Node_id annotation = ast.child( decl, 0 );

    // `auto` has no annotation node at all, so guard before asking.
    return annotation.is_valid() && types.type_of( annotation ).is_valid();
}

bool is_const_binding( const Ast& ast, Node_id decl )
{
    // Every one of these carries its binding on children[0]: a variable's annotation, a parameter's
    // type, a function's return type - and a method's, which is why a Method_decl belongs here even
    // though the *receiver's* constness is a different question, which is_const_method asks.
    const bool declares_a_binding =
        decl.is_valid() && ( ast.kind( decl ) == Node_kind::Var_decl || ast.kind( decl ) == Node_kind::Param_decl ||
                             ast.kind( decl ) == Node_kind::Function_decl || ast.kind( decl ) == Node_kind::Method_decl );

    if( !declares_a_binding )
    {
        return false;
    }

    const Node_id annotation = ast.child( decl, 0 );
    return annotation.is_valid() && ast.kind( annotation ) == Node_kind::Const_type;
}

bool is_const_method( const Ast& ast, Node_id method )
{
    if( !method.is_valid() || ast.kind( method ) != Node_kind::Method_decl )
    {
        return false;
    }

    // The trailing `const` marks the **receiver**, which the parser wraps as `const ref T` - child
    // 0 of the method is its return type, which is a different `const` entirely. So this is the
    // ordinary const-binding question, asked of parameter 0.
    const std::span<const Node_id> params = ast.children( ast.child( method, 1 ) );

    return has_receiver( ast, method ) && is_const_binding( ast, params[0] );
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

    const std::vector<Node_id> parameters = type_parameters( ast, ast.type_param_list( table.get( aggregate ).declaration ) );

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

bool has_destructor( const Ast& ast, Node_id declaration )
{
    if( !declaration.is_valid() || !is_aggregate( ast.kind( declaration ) ) )
    {
        return false;
    }

    for( const Node_id member : ast.members( declaration ) )
    {
        if( ast.kind( member ) == Node_kind::Destructor_decl )
        {
            return true;
        }
    }

    return false;
}

namespace
{
// The recursion behind instance_owns. `visiting` is not a cycle *check* - order_structs already
// refuses a by-value cycle between declarations - but instances are interned as they are asked
// about, and answering the same one twice down a chain would not terminate.
bool owns_through_fields(
    const Ast& ast, Type_table& table, Type_id instance, std::span<const Type_id> recorded, std::vector<Type_id>& visiting
)
{
    if( !instance.is_valid() || table.is_error( instance ) || !table.is_struct( instance ) )
    {
        return false;
    }

    const Node_id declaration = table.get( instance ).declaration;

    if( !declaration.is_valid() )
    {
        return false;
    }

    if( has_destructor( ast, declaration ) )
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
    for( const Node_id field : ast.members( declaration ) )
    {
        if( ast.kind( field ) != Node_kind::Field_decl )
        {
            continue;
        }

        if( owns_through_fields( ast, table, field_type( ast, table, instance, field, recorded ), recorded, visiting ) )
        {
            visiting.pop_back();
            return true;
        }
    }

    visiting.pop_back();
    return false;
}
} // namespace

bool instance_owns( const Ast& ast, Type_table& table, Type_id instance, std::span<const Type_id> recorded )
{
    std::vector<Type_id> visiting;

    return owns_through_fields( ast, table, instance, recorded, visiting );
}

Type_id binding_type( const Ast& ast, const Types& types, Node_id param )
{
    return is_borrowed_binding( ast, types, param ) ? types.type_of( ast.child( param, 0 ) ) : types.type_of( param );
}

Node_id unwrap_const( const Ast& ast, Node_id annotation )
{
    return annotation.is_valid() && ast.kind( annotation ) == Node_kind::Const_type ? ast.child( annotation, 0 ) : annotation;
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

    SECTION( "and a parse error does not reach the checker at all" )
    {
        const Typed p( "i32 main() { return 1 +; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }
}

} // namespace keel
#endif
