#include "sema/aggregates.h"

#include <algorithm>

#include <fmt/format.h>

#include "sema/type_checker.h"

namespace keel::sema
{

std::vector<Node_id> Aggregates::order_structs()
{
    std::vector<Node_id> cycle_reported {};

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

        bool already_reported = std::find( cycle_reported.begin(), cycle_reported.end(), child ) != cycle_reported.end();
        if( already_reported )
        {
            continue;
        }

        std::vector<Node_id> path;

        if( !contains_itself( child, path ) )
        {
            continue;
        }

        report_containment_cycle( child, path );

        // One message covers the whole cycle; unmarked, `A -> B -> A` is found again from B.
        cycle_reported.insert( cycle_reported.end(), path.begin(), path.end() );
    }

    return cycle_reported;
}

void Aggregates::report_containment_cycle( Node_id decl, const std::vector<Node_id>& path )
{
    std::string route;

    for( const Node_id node : path )
    {
        if( !route.empty() )
        {
            route += " -> ";
        }

        route += interner_.text( Symbol_id { ast_.aux( node ) } );
    }

    reporter_.error_at(
        ast_.span( decl ),
        fmt::format( "`{}` contains itself, so it has no size", interner_.text( Symbol_id { ast_.aux( decl ) } ) ),
        route
    );
}

bool Aggregates::contains_itself( Node_id decl, std::vector<Node_id>& path )
{
    // Being in the order *is* being done: a struct lands there only once everything it contains
    // has, so membership and "proved acyclic" are the same fact.
    if( std::find( struct_order_.begin(), struct_order_.end(), decl ) != struct_order_.end() )
    {
        return false;
    }

    path.push_back( decl );

    for( Node_id field : ast_.members( decl ) )
    {
        if( ast_.kind( field ) != Node_kind::Field_decl )
        {
            continue;
        }

        // Read back what the field pass recorded. Calling Annotations::type_of again would report
        // every unknown type and D1 suggestion a second time.
        const Type_id field_type = types_.type_of( field );

        // A pointer to a struct is finite, so only by-value containment counts - which is what
        // makes `struct Node { Node* next; }` legal once pointers arrive at M3.
        if( !field_type.is_valid() || table_.is_error( field_type ) || !table_.is_struct( field_type ) )
        {
            continue;
        }

        const Node_id field_decl = table_.get( field_type ).declaration;
        if( !field_decl.is_valid() || !is_aggregate( ast_.kind( field_decl ) ) )
        {
            continue;
        }

        if( std::find( path.begin(), path.end(), field_decl ) != path.end() )
        {
            path.push_back( field_decl );
            return true;
        }

        if( contains_itself( field_decl, path ) )
        {
            return true;
        }
    }

    // The DFS post-order: every struct after everything it contains, which is the order C needs for
    // by-value members. A generic aggregate is walked for the cycle above and then left out - it has
    // no layout of its own, and what gets ordered and emitted is each instantiation.
    if( !is_generic( ast_, decl ) )
    {
        struct_order_.push_back( decl );
    }
    path.pop_back();
    return false;
}

void Aggregates::compute_owning()
{
    // struct_order_ is the DFS post-order, so a field's answer is already in owning_ when its owner
    // is reached - which is what makes one forward pass enough. A cycle never enters the order.
    for( const Node_id decl : struct_order_ )
    {
        bool is_owning = keel::has_destructor( ast_, decl );

        for( const Node_id field : ast_.members( decl ) )
        {
            if( is_owning )
            {
                break;
            }

            if( ast_.kind( field ) != Node_kind::Field_decl )
            {
                continue;
            }

            const Type_id field_type = types_.type_of( field );

            is_owning = owns( field_type );
        }

        if( is_owning )
        {
            owning_.insert( decl.v );
        }
    }
}

bool Aggregates::owns( Type_id type ) const
{
    if( !type.is_valid() || table_.is_error( type ) || !table_.is_struct( type ) )
    {
        return false;
    }

    const Node_id declaration = table_.get( type ).declaration;

    return declaration.is_valid() && owning_.contains( declaration.v );
}

Node_id Aggregates::find_member( Node_id decl, Node_kind kind ) const
{
    for( const Node_id member : ast_.members( decl ) )
    {
        if( ast_.kind( member ) == kind )
        {
            return member;
        }
    }

    return Node_id {};
}

// `decl` is the aggregate, not the call. Only a Method_decl is reachable by name: a constructor
// shares the type's name and is reached by calling the type, and a destructor is never called at
// all - so matching either here would let `p.Point()` resolve.
Node_id Aggregates::find_method( Node_id decl, Symbol_id name ) const
{
    if( !decl.is_valid() )
    {
        return Node_id {};
    }

    for( const Node_id member : ast_.members( decl ) )
    {
        if( ast_.kind( member ) == Node_kind::Method_decl && Symbol_id { ast_.aux( member ) } == name )
        {
            return member;
        }
    }

    return Node_id {};
}

Node_id Aggregates::find_field( Type_id type, Symbol_id name ) const
{
    const Node_id decl = table_.get( type ).declaration;

    if( !decl.is_valid() )
    {
        return Node_id();
    }

    for( const Node_id field : ast_.members( decl ) )
    {
        // The kind matters as much as the name. A constructor's aux is the *type's* name, so
        // without this a search for a field called `B` finds `B`'s constructor - which is how
        // `b.B( 2 )` came to report that `B` was a field of itself.
        if( ast_.kind( field ) == Node_kind::Field_decl && ast_.aux( field ) == name.v )
        {
            return field;
        }
    }

    return Node_id();
}

Bindings Aggregates::bindings_of( Type_id aggregate ) const
{
    return aggregate_bindings( ast_, table_, aggregate, types_.recorded() );
}

Type_id Aggregates::field_type( Type_id aggregate, Node_id field )
{
    return keel::field_type( ast_, table_, aggregate, field, types_.recorded() );
}

std::vector<Node_id> Aggregates::take_struct_order()
{
    return std::move( struct_order_ );
}

std::unordered_set<u32> Aggregates::take_owning()
{
    return std::move( owning_ );
}

} // namespace keel::sema

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "lex/lexer.h"
#include "parse/parser.h"

namespace keel
{
namespace
{

// Aggregates needs a parsed tree and the type of every field, and nothing else - no resolver, no
// checker, no expression walk. So the fixture supplies exactly that: it interns one struct type
// per top-level aggregate and records a field's type when its annotation names one. That is the
// point of the class existing, and the fixture is the proof.
class Shapes
{
public:
    explicit Shapes( std::string_view source )
        : ast_(
              parse( lex( sm_.add_file( "t.kl", std::string( source ) ), sm_, interner_, literal_pool_, diags_ ), sm_, diags_ )
          ),
          reporter_( sm_, diags_ ),
          aggregates_( ast_, interner_, types_, reporter_ )
    {
        types_.size_to( ast_.node_count() );

        std::unordered_map<u32, Type_id> by_name;

        for( const Node_id decl : ast_.children( ast_.root() ) )
        {
            if( is_aggregate( ast_.kind( decl ) ) )
            {
                const std::string_view name = interner_.text( Symbol_id { ast_.aux( decl ) } );
                by_name[ast_.aux( decl )]   = types_.table().structure( decl, {}, name );
            }
        }

        for( const Node_id decl : ast_.children( ast_.root() ) )
        {
            for( const Node_id field : ast_.members( decl ) )
            {
                if( ast_.kind( field ) != Node_kind::Field_decl )
                {
                    continue;
                }

                const Node_id annotation = ast_.child( field, 0 );

                if( ast_.kind( annotation ) != Node_kind::Named_type )
                {
                    continue;
                }

                const auto named = by_name.find( ast_.aux( annotation ) );

                types_.record(
                    field,
                    named != by_name.end()
                        ? named->second
                        : types_.table().from_spelling( interner_.text( Symbol_id { ast_.aux( annotation ) } ) )
                );
            }
        }
    }

    sema::Aggregates& aggregates()
    {
        return aggregates_;
    }

    // The nth top-level aggregate in source order, which is not the order it gets ordered into.
    Node_id declaration( std::size_t index ) const
    {
        for( const Node_id decl : ast_.children( ast_.root() ) )
        {
            if( is_aggregate( ast_.kind( decl ) ) && index-- == 0 )
            {
                return decl;
            }
        }
        return Node_id {};
    }

    Type_id type_of( Node_id decl )
    {
        return types_.table().structure( decl, {}, interner_.text( Symbol_id { ast_.aux( decl ) } ) );
    }

    Node_kind kind( Node_id id ) const
    {
        return ast_.kind( id );
    }

    Symbol_id symbol( std::string_view name )
    {
        return interner_.intern( name );
    }

    std::size_t errors() const
    {
        return diags_.error_count();
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

    sema::Types_builder types_;
    sema::Reporter      reporter_;
    sema::Aggregates    aggregates_;
};

TEST_CASE( "aggregates_orders_an_aggregate_after_everything_it_contains", "[sema][aggregates]" )
{
    // Written outer-first, so source order and containment order disagree.
    Shapes s( "struct Outer { Inner i; };\nstruct Inner { i32 x; };" );

    const std::vector<Node_id> reported = s.aggregates().order_structs();
    REQUIRE( reported.empty() );

    const std::vector<Node_id> order = s.aggregates().take_struct_order();
    REQUIRE( order.size() == 2 );
    REQUIRE( order[0] == s.declaration( 1 ) ); // Inner
    REQUIRE( order[1] == s.declaration( 0 ) ); // Outer
}

TEST_CASE( "aggregates_report_a_containment_cycle_once_and_name_the_route", "[sema][aggregates]" )
{
    Shapes s( "struct A { B b; };\nstruct B { A a; };" );

    const std::vector<Node_id> reported = s.aggregates().order_structs();

    INFO( s.rendered() );
    REQUIRE( s.errors() == 1 ); // one mistake, not one per member
    REQUIRE( s.rendered().find( "A -> B -> A" ) != std::string::npos );

    // Both are handed back so the caller can keep its own rules quiet about them.
    REQUIRE( reported.size() == 3 );
    REQUIRE( s.aggregates().take_struct_order().empty() ); // nothing on a cycle is ordered
}

// A generic aggregate has no layout of its own, the way a generic function has no code: it is
// walked for the cycle and then left out, and what gets ordered is each instantiation.
TEST_CASE( "aggregates_leave_a_generic_aggregate_out_of_the_order", "[sema][aggregates][generic]" )
{
    Shapes s( "struct Box<T> { T v; };\nstruct Plain { i32 x; };" );

    REQUIRE( s.aggregates().order_structs().empty() );

    const std::vector<Node_id> order = s.aggregates().take_struct_order();
    REQUIRE( order.size() == 1 );
    REQUIRE( order[0] == s.declaration( 1 ) ); // Plain, not Box
}

TEST_CASE( "aggregates_own_through_a_destructor_and_through_a_field", "[sema][aggregates][owning]" )
{
    Shapes s( "class Res { i32 h; ~Res() { } };\nclass Holder { Res r; };\nstruct Plain { i32 x; };" );

    s.aggregates().order_structs();
    s.aggregates().compute_owning();

    REQUIRE( s.aggregates().owns( s.type_of( s.declaration( 0 ) ) ) );  // its own destructor
    REQUIRE( s.aggregates().owns( s.type_of( s.declaration( 1 ) ) ) );  // a field that owns
    REQUIRE( !s.aggregates().owns( s.type_of( s.declaration( 2 ) ) ) ); // neither
    REQUIRE( !s.aggregates().owns( Type_id {} ) );
}

TEST_CASE( "aggregates_find_a_member_a_method_and_a_field", "[sema][aggregates]" )
{
    Shapes s( "struct B { i32 B; i32 twice() { return 0; } B( i32 v ) { } };" );

    const Node_id decl = s.declaration( 0 );

    REQUIRE( s.aggregates().find_member( decl, Node_kind::Constructor_decl ).is_valid() );
    REQUIRE( !s.aggregates().find_member( decl, Node_kind::Destructor_decl ).is_valid() );
    REQUIRE( s.aggregates().find_method( decl, s.symbol( "twice" ) ).is_valid() );
    REQUIRE( !s.aggregates().find_method( decl, s.symbol( "B" ) ).is_valid() ); // the constructor is not a method

    // A constructor's aux is the type's name, so a field search that ignored the kind would find
    // it - which is how `b.B( 2 )` came to report that `B` was a field of itself.
    const Node_id field = s.aggregates().find_field( s.type_of( decl ), s.symbol( "B" ) );
    REQUIRE( s.kind( field ) == Node_kind::Field_decl );
}

} // namespace
} // namespace keel
#endif
