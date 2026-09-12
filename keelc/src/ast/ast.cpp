#include "ast/ast.h"
#include <cassert>

namespace keel
{

Node_id Ast::add( Node_kind kind, Span span, u32 aux, std::span<const Node_id> children )
{
    std::size_t offset = children_.size();
    children_.insert( children_.end(), children.begin(), children.end() );
    Node node { kind, span, aux, narrow_cast<u32>( offset ), narrow_cast<u32>( children.size() ) };
    nodes_.push_back( node );

    return Node_id { narrow_cast<u32>( nodes_.size() - 1 ) };
}

Node_kind Ast::kind( Node_id id ) const
{
    assert( id.is_valid() && id.v < nodes_.size() && "invalid node id" );
    return nodes_[id.v].kind;
}

Span Ast::span( Node_id id ) const
{
    assert( id.is_valid() && id.v < nodes_.size() && "invalid node id" );
    return nodes_[id.v].span;
}

u32 Ast::aux( Node_id id ) const
{
    assert( id.is_valid() && id.v < nodes_.size() && "invalid node id" );
    return nodes_[id.v].aux;
}

std::span<const Node_id> Ast::children( Node_id id ) const
{
    assert( id.is_valid() && id.v < nodes_.size() && "invalid node id" );
    const Node& node = nodes_[id.v];

    // Note: this span is invalidated by any later add() call
    return std::span<const Node_id>( children_.data() + node.first_child, node.child_count );
}

Node_id Ast::root() const
{
    return root_;
}

void Ast::set_root( Node_id id )
{
    root_ = id;
}

std::size_t Ast::node_count() const
{
    return nodes_.size();
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace keel
{
namespace
{

constexpr File_id k_file { 0 };

Span at( u32 start, u32 end )
{
    return Span { k_file, start, end };
}

} // namespace

TEST_CASE( "ast_starts_empty", "[ast]" )
{
    const Ast ast;

    REQUIRE( ast.node_count() == 0 );
    REQUIRE_FALSE( ast.root().is_valid() );
}

TEST_CASE( "ast_add_returns_dense_sequential_ids", "[ast]" )
{
    Ast ast;

    const Node_id a = ast.add( Node_kind::Int_literal, at( 0, 1 ), 0, {} );
    const Node_id b = ast.add( Node_kind::Int_literal, at( 2, 3 ), 0, {} );
    const Node_id c = ast.add( Node_kind::Int_literal, at( 4, 5 ), 0, {} );

    REQUIRE( a.is_valid() );
    REQUIRE( a.v == 0 );
    REQUIRE( b.v == 1 );
    REQUIRE( c.v == 2 );
    REQUIRE( ast.node_count() == 3 );
}

TEST_CASE( "ast_round_trips_node_fields", "[ast]" )
{
    Ast ast;

    const Node_id id = ast.add( Node_kind::Named_type, at( 10, 13 ), 7, {} );

    REQUIRE( ast.kind( id ) == Node_kind::Named_type );
    REQUIRE( ast.span( id ) == at( 10, 13 ) );
    REQUIRE( ast.aux( id ) == 7 );
    REQUIRE( ast.children( id ).empty() );
}

// A Param_list with no parameters, or a Block with no statements - the common case, not an edge one.
TEST_CASE( "ast_empty_child_list", "[ast]" )
{
    Ast ast;

    const Node_id id = ast.add( Node_kind::Param_list, at( 8, 10 ), 0, {} );

    REQUIRE( ast.children( id ).empty() );
    REQUIRE( ast.children( id ).size() == 0 );
}

TEST_CASE( "ast_children_round_trip", "[ast]" )
{
    Ast ast;

    const Node_id first  = ast.add( Node_kind::Int_literal, at( 0, 1 ), 0, {} );
    const Node_id second = ast.add( Node_kind::Int_literal, at( 2, 3 ), 0, {} );
    const Node_id block  = ast.add( Node_kind::Block, at( 0, 4 ), 0, { first, second } );

    const auto kids = ast.children( block );

    REQUIRE( kids.size() == 2 );
    REQUIRE( kids[0] == first );
    REQUIRE( kids[1] == second );
}

// children_ holds every list back to back, so a bug in the offset or count shows up as one node
// seeing another's children.
TEST_CASE( "ast_child_lists_do_not_interfere", "[ast]" )
{
    Ast ast;

    std::vector<Node_id> leaves;
    for( u32 i = 0; i < 12; ++i )
    {
        leaves.push_back( ast.add( Node_kind::Int_literal, at( i, i + 1 ), i, {} ) );
    }

    const Node_id a = ast.add( Node_kind::Block, at( 0, 1 ), 0, { leaves[0], leaves[1], leaves[2] } );
    const Node_id b = ast.add( Node_kind::Block, at( 1, 2 ), 0, { leaves[3] } );
    const Node_id c = ast.add( Node_kind::Block, at( 2, 3 ), 0, {} );
    const Node_id d = ast.add( Node_kind::Block, at( 3, 4 ), 0, { leaves[4], leaves[5] } );

    REQUIRE( ast.children( a ).size() == 3 );
    REQUIRE( ast.children( b ).size() == 1 );
    REQUIRE( ast.children( c ).size() == 0 );
    REQUIRE( ast.children( d ).size() == 2 );

    REQUIRE( ast.children( a )[2] == leaves[2] );
    REQUIRE( ast.children( b )[0] == leaves[3] );
    REQUIRE( ast.children( d )[0] == leaves[4] );
    REQUIRE( ast.children( d )[1] == leaves[5] );
}

// The span children() returns points into children_, which reallocates. Re-fetching after more
// adds must still give the right list - that is what the offset/count are for.
TEST_CASE( "ast_children_remain_correct_after_growth", "[ast]" )
{
    Ast ast;

    const Node_id x     = ast.add( Node_kind::Int_literal, at( 0, 1 ), 1, {} );
    const Node_id y     = ast.add( Node_kind::Int_literal, at( 2, 3 ), 2, {} );
    const Node_id early = ast.add( Node_kind::Block, at( 0, 4 ), 0, { x, y } );

    for( u32 i = 0; i < 1000; ++i )
    {
        const Node_id leaf = ast.add( Node_kind::Int_literal, at( i, i + 1 ), i, {} );
        ast.add( Node_kind::Block, at( i, i + 2 ), 0, { leaf, leaf } );
    }

    const auto kids = ast.children( early );

    REQUIRE( kids.size() == 2 );
    REQUIRE( kids[0] == x );
    REQUIRE( kids[1] == y );
}

// Fixed arity with an invalid child in the slot, rather than a shorter list: If_stmt always has
// three children and children()[2].is_valid() says whether there is an else.
TEST_CASE( "ast_invalid_child_marks_an_absent_optional", "[ast]" )
{
    Ast ast;

    const Node_id cond = ast.add( Node_kind::Int_literal, at( 0, 1 ), 0, {} );
    const Node_id then = ast.add( Node_kind::Block, at( 2, 4 ), 0, {} );
    const Node_id stmt = ast.add( Node_kind::Return_stmt, at( 0, 4 ), 0, { cond, then, Node_id {} } );

    const auto kids = ast.children( stmt );

    REQUIRE( kids.size() == 3 );
    REQUIRE( kids[0].is_valid() );
    REQUIRE( kids[1].is_valid() );
    REQUIRE_FALSE( kids[2].is_valid() );
}

TEST_CASE( "ast_root_is_settable", "[ast]" )
{
    Ast ast;

    REQUIRE_FALSE( ast.root().is_valid() );

    const Node_id file = ast.add( Node_kind::Source_file, at( 0, 20 ), 0, {} );
    ast.set_root( file );

    REQUIRE( ast.root() == file );
    REQUIRE( ast.kind( ast.root() ) == Node_kind::Source_file );
}

TEST_CASE( "ast_accepts_a_span_as_well_as_a_braced_list", "[ast]" )
{
    Ast ast;

    const Node_id a = ast.add( Node_kind::Int_literal, at( 0, 1 ), 0, {} );
    const Node_id b = ast.add( Node_kind::Int_literal, at( 2, 3 ), 0, {} );

    // The parser collects variable-length lists into a scratch vector and hands the whole thing over.
    const std::vector<Node_id> scratch { a, b };
    const Node_id              list = ast.add( Node_kind::Param_list, at( 0, 4 ), 0, scratch );

    REQUIRE( ast.children( list ).size() == 2 );
    REQUIRE( ast.children( list )[0] == a );
}

// The shape the parser must produce for `i32 main() { return 0; }`, built by hand. Also the tree
// the dumper will be developed against.
TEST_CASE( "ast_builds_a_small_function", "[ast]" )
{
    Ast ast;

    const Node_id ret_type = ast.add( Node_kind::Named_type, at( 0, 3 ), 0, {} );
    const Node_id params   = ast.add( Node_kind::Param_list, at( 8, 10 ), 0, {} );
    const Node_id zero     = ast.add( Node_kind::Int_literal, at( 20, 21 ), 0, {} );
    const Node_id ret      = ast.add( Node_kind::Return_stmt, at( 13, 22 ), 0, { zero } );
    const Node_id body     = ast.add( Node_kind::Block, at( 11, 24 ), 0, { ret } );
    const Node_id func     = ast.add( Node_kind::Function_decl, at( 0, 24 ), 0, { ret_type, params, body } );
    const Node_id file     = ast.add( Node_kind::Source_file, at( 0, 24 ), 0, { func } );

    ast.set_root( file );

    REQUIRE( ast.node_count() == 7 );
    REQUIRE( ast.kind( ast.root() ) == Node_kind::Source_file );

    const Node_id only = ast.children( ast.root() )[0];
    REQUIRE( ast.kind( only ) == Node_kind::Function_decl );

    // Function_decl arity is fixed at three: return type, parameter list, body.
    REQUIRE( ast.children( only ).size() == 3 );
    REQUIRE( ast.kind( ast.children( only )[0] ) == Node_kind::Named_type );
    REQUIRE( ast.kind( ast.children( only )[1] ) == Node_kind::Param_list );
    REQUIRE( ast.kind( ast.children( only )[2] ) == Node_kind::Block );

    const Node_id block = ast.children( only )[2];
    REQUIRE( ast.children( block ).size() == 1 );
    REQUIRE( ast.kind( ast.children( block )[0] ) == Node_kind::Return_stmt );
    REQUIRE( ast.span( func ) == at( 0, 24 ) );
}

TEST_CASE( "node_kind_name_covers_every_kind", "[ast]" )
{
    for( u16 i = 0; i < static_cast<u16>( Node_kind::Count ); ++i )
    {
        const auto kind = static_cast<Node_kind>( i );
        INFO( "kind index " << i );
        REQUIRE_FALSE( node_kind_name( kind ).empty() );
        REQUIRE( node_kind_name( kind ) != "Unknown" );
    }
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
