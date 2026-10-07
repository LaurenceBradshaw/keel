// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/ast.h"
#include <algorithm>
#include <cassert>

namespace keel
{

Node_id Ast::add( Node_kind kind, Span span, u32 aux, std::span<const Node_id> children )
{
    std::size_t offset = children_.size();
    children_.insert( children_.end(), children.begin(), children.end() );
    Node node { kind, span, aux, narrow_cast<u32>( offset ), narrow_cast<u32>( children.size() ) };
    nodes_.push_back( node );

    const bool is_broken = kind == Node_kind::Error || failed_within( span ) ||
                           std::any_of( children.begin(), children.end(), [this]( Node_id id ) { return broken( id ); } );

    broken_.push_back( is_broken );

    return Node_id { narrow_cast<u32>( nodes_.size() - 1 ) };
}

void Ast::set_root( Node_id id )
{
    root_ = id;
}

void Ast::set_access( Node_id id, Access access )
{
    access_[id.v] = access;
}

void Ast::set_name_span( Node_id decl, Span span )
{
    name_spans_[decl.v] = span;
}

void Ast::set_type_name_span( Node_id literal, Span span )
{
    type_name_spans_[literal.v] = span;
}

Node_id Ast::root() const
{
    return root_;
}

std::size_t Ast::node_count() const
{
    return nodes_.size();
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

    return std::span<const Node_id>( children_.data() + node.first_child, node.child_count );
}

Node_id Ast::child( Node_id id, std::size_t index ) const
{
    const std::span<const Node_id> all = children( id );

    assert( index < all.size() && "child index out of range for this node kind" );

    return all[index];
}

Access Ast::access( Node_id id ) const
{
    const auto it = access_.find( id.v );
    return it != access_.end() ? it->second : Access::Public;
}

Span Ast::name_span( Node_id decl ) const
{
    const auto it = name_spans_.find( decl.v );
    return it != name_spans_.end() ? it->second : span( decl );
}

Span Ast::type_name_span( Node_id literal ) const
{
    assert( kind( literal ) == Node_kind::Struct_literal && "type name spans are a struct literal's, not any node's" );
    const auto it = type_name_spans_.find( literal.v );
    return it != type_name_spans_.end() ? it->second : Span::none();
}

void Ast::fail( Span at )
{
    failures_.push_back( at );
}

bool Ast::broken( Node_id id ) const
{
    return id.is_valid() && id.v < broken_.size() && broken_[id.v];
}

bool Ast::failed_within( Span span ) const
{
    for( auto it = failures_.rbegin(); it != failures_.rend(); ++it )
    {
        if( it->file == span.file && it->start >= span.start && it->start <= span.end )
        {
            return true;
        }
    }

    return false;
}

Symbol_id Ast::name( Node_id id ) const
{
    switch( kind( id ) )
    {
    case Node_kind::Error:
    case Node_kind::Import_decl:
    case Node_kind::Function_decl:
    case Node_kind::Destructor_decl:
    case Node_kind::Constructor_decl:
    case Node_kind::Method_decl:
    case Node_kind::Param_decl:
    case Node_kind::Named_type:
    case Node_kind::Name_expr:
    case Node_kind::Var_decl:
    case Node_kind::Struct_decl:
    case Node_kind::Class_decl:
    case Node_kind::Enum_decl:
    case Node_kind::Variant_decl:
    case Node_kind::Path_expr:
    case Node_kind::Field_decl:
    case Node_kind::Field_expr:
    case Node_kind::Struct_literal:
    case Node_kind::Field_init:
    case Node_kind::Binding_decl:
    case Node_kind::Type_param_decl:
    case Node_kind::Where_clause:
    case Node_kind::Bound_name:
        return Symbol_id { aux( id ) };
    default:
        assert( false && "this kind's aux is not a name" );
        return Symbol_id {};
    }
}

Token_kind Ast::op( Node_id id ) const
{
    assert(
        ( kind( id ) == Node_kind::Binary_expr || kind( id ) == Node_kind::Unary_expr || kind( id ) == Node_kind::Assign_stmt ||
          kind( id ) == Node_kind::Increment_stmt ) &&
        "this kind's aux is not an operator"
    );
    return static_cast<Token_kind>( aux( id ) );
}

Keyword Ast::keyword( Node_id id ) const
{
    assert(
        ( kind( id ) == Node_kind::Mode_type || kind( id ) == Node_kind::Marker_expr || kind( id ) == Node_kind::Cast_expr ) &&
        "this kind's aux is not a keyword"
    );
    return static_cast<Keyword>( aux( id ) );
}

Literal_id Ast::literal( Node_id id ) const
{
    assert(
        ( kind( id ) == Node_kind::Int_literal || kind( id ) == Node_kind::Float_literal ||
          kind( id ) == Node_kind::String_literal || kind( id ) == Node_kind::Char_literal ) &&
        "this kind's aux is not a literal"
    );
    return Literal_id { aux( id ) };
}

bool Ast::is_true( Node_id literal ) const
{
    assert( kind( literal ) == Node_kind::Bool_literal );
    return aux( literal ) != 0;
}

bool Ast::is_unsafe( Node_id block ) const
{
    assert( kind( block ) == Node_kind::Block );
    return aux( block ) != 0;
}

bool Ast::is_default_arm( Node_id arm ) const
{
    assert( kind( arm ) == Node_kind::Case_arm );
    return aux( arm ) != 0;
}

std::span<const Node_id> Ast::declarations( Node_id source_file ) const
{
    assert( kind( source_file ) == Node_kind::Source_file );
    return children( source_file );
}

Node_id Ast::package( Node_id id ) const
{
    const std::span<const Node_id> all = children( id );

    switch( kind( id ) )
    {
    case Node_kind::Import_decl:
    case Node_kind::Named_type:
        return all.empty() ? Node_id {} : all[0];
    case Node_kind::Struct_literal:
        return !all.empty() && kind( all[0] ) == Node_kind::Name_expr ? all[0] : Node_id {};
    default:
        assert( false && "this kind has no package" );
        return Node_id {};
    }
}

Node_id Ast::return_type( Node_id id ) const
{
    assert( ( is_function_like( kind( id ) ) || kind( id ) == Node_kind::Function_type ) && "no return type here" );
    return child( id, 0 );
}

Node_id Ast::param_list( Node_id id ) const
{
    assert( ( is_function_like( kind( id ) ) || kind( id ) == Node_kind::Function_type ) && "no parameters here" );
    return child( id, 1 );
}

std::span<const Node_id> Ast::params( Node_id id ) const
{
    return children( param_list( id ) );
}

std::span<const Node_id> Ast::explicit_params( Node_id fn ) const
{
    assert( is_function_like( kind( fn ) ) );
    const std::span<const Node_id> all = params( fn );

    return !all.empty() && name( all[0] ) == Interner::keyword( Keyword::This ) ? all.subspan( 1 ) : all;
}

Node_id Ast::body( Node_id id ) const
{
    switch( kind( id ) )
    {
    case Node_kind::While_stmt:
        return child( id, 1 );
    case Node_kind::For_stmt:
        return child( id, 3 );
    case Node_kind::Case_arm:
        return children( id ).back();
    default:
        assert( is_function_like( kind( id ) ) && "this kind has no body" );
        return child( id, 2 );
    }
}

Node_id Ast::type_param_list( Node_id id ) const
{
    if( is_aggregate( kind( id ) ) || kind( id ) == Node_kind::Enum_decl )
    {
        return child( id, 0 );
    }

    return is_function_like( kind( id ) ) ? child( id, 3 ) : Node_id {};
}

// The parameters come before the clauses. An invalid list has neither.
static std::size_t first_where_clause( const Ast& ast, Node_id list )
{
    const std::span<const Node_id> all = ast.children( list );

    return static_cast<std::size_t>(
        std::find_if( all.begin(), all.end(), [&]( Node_id n ) { return ast.kind( n ) == Node_kind::Where_clause; } ) -
        all.begin()
    );
}

std::span<const Node_id> Ast::type_param_decls( Node_id list ) const
{
    if( !list.is_valid() )
    {
        return {};
    }

    assert( kind( list ) == Node_kind::Type_param_list );
    return children( list ).first( first_where_clause( *this, list ) );
}

std::span<const Node_id> Ast::where_clauses( Node_id list ) const
{
    if( !list.is_valid() )
    {
        return {};
    }

    assert( kind( list ) == Node_kind::Type_param_list );
    return children( list ).subspan( first_where_clause( *this, list ) );
}

std::span<const Node_id> Ast::bounds( Node_id where_clause ) const
{
    assert( kind( where_clause ) == Node_kind::Where_clause );
    return children( where_clause );
}

Node_id Ast::type_arg_list( Node_id id ) const
{
    const std::span<const Node_id> all = children( id );

    switch( kind( id ) )
    {
    case Node_kind::Generic_type:
        return all[1];
    case Node_kind::Call_expr:
        return all[2];
    case Node_kind::Name_expr:
        return all.empty() ? Node_id {} : all[0];
    case Node_kind::Path_expr:
        return all.size() > 1 ? all[1] : Node_id {};
    default:
        assert( false && "this kind has no type arguments" );
        return Node_id {};
    }
}

std::span<const Node_id> Ast::type_args( Node_id id ) const
{
    const Node_id list = type_arg_list( id );
    return list.is_valid() ? children( list ) : std::span<const Node_id> {};
}

std::span<const Node_id> Ast::members( Node_id id ) const
{
    assert( is_aggregate( kind( id ) ) && "members are a struct's or a class's, not any node's" );
    return children( id ).subspan( 1 ); // past the Type_param_list slot
}

std::span<const Node_id> Ast::variants( Node_id id ) const
{
    assert( kind( id ) == Node_kind::Enum_decl && "variants are an enum's, not any node's" );
    return children( id ).subspan( 2 ); // past the Type_param_list and underlying-type slots
}

Node_id Ast::underlying_type( Node_id enum_decl ) const
{
    assert( kind( enum_decl ) == Node_kind::Enum_decl );
    return child( enum_decl, 1 );
}

std::span<const Node_id> Ast::payload( Node_id variant ) const
{
    assert( kind( variant ) == Node_kind::Variant_decl );
    return children( variant );
}

Node_id Ast::annotation( Node_id decl ) const
{
    assert(
        ( kind( decl ) == Node_kind::Param_decl || kind( decl ) == Node_kind::Var_decl || kind( decl ) == Node_kind::Field_decl
        ) &&
        "this kind has no annotation"
    );
    return child( decl, 0 );
}

Node_id Ast::initialiser( Node_id decl ) const
{
    assert( kind( decl ) == Node_kind::Var_decl );
    return child( decl, 1 );
}

Node_id Ast::declared_type( Node_id decl ) const
{
    switch( kind( decl ) )
    {
    case Node_kind::Function_type:
        return return_type( decl );
    case Node_kind::Field_type:
        return child( decl, 0 );
    default:
        return is_function_like( kind( decl ) ) ? return_type( decl ) : annotation( decl );
    }
}

Node_id Ast::inner_type( Node_id id ) const
{
    assert(
        ( kind( id ) == Node_kind::Pointer_type || kind( id ) == Node_kind::Many_pointer_type ||
          kind( id ) == Node_kind::Mode_type || kind( id ) == Node_kind::Const_type ) &&
        "this kind wraps no type"
    );
    return child( id, 0 );
}

Node_id Ast::generic_name( Node_id generic_type ) const
{
    assert( kind( generic_type ) == Node_kind::Generic_type );
    return child( generic_type, 0 );
}

Node_id Ast::lhs( Node_id binary ) const
{
    assert( kind( binary ) == Node_kind::Binary_expr );
    return child( binary, 0 );
}

Node_id Ast::rhs( Node_id binary ) const
{
    assert( kind( binary ) == Node_kind::Binary_expr );
    return child( binary, 1 );
}

Node_id Ast::operand( Node_id id ) const
{
    switch( kind( id ) )
    {
    case Node_kind::Cast_expr:
        return child( id, 1 );
    default:
        assert(
            ( kind( id ) == Node_kind::Unary_expr || kind( id ) == Node_kind::Marker_expr ||
              kind( id ) == Node_kind::Increment_stmt ) &&
            "this kind has no operand"
        );
        return child( id, 0 );
    }
}

Node_id Ast::callee( Node_id call ) const
{
    assert( kind( call ) == Node_kind::Call_expr );
    return child( call, 0 );
}

Node_id Ast::arg_list( Node_id call ) const
{
    assert( kind( call ) == Node_kind::Call_expr );
    return child( call, 1 );
}

std::span<const Node_id> Ast::arguments( Node_id call ) const
{
    return children( arg_list( call ) );
}

Node_id Ast::object( Node_id id ) const
{
    assert( ( kind( id ) == Node_kind::Field_expr || kind( id ) == Node_kind::Index_expr ) && "this kind has no object" );
    return child( id, 0 );
}

Node_id Ast::index( Node_id index_expr ) const
{
    assert( kind( index_expr ) == Node_kind::Index_expr );
    return child( index_expr, 1 );
}

Node_id Ast::qualifier( Node_id path ) const
{
    assert( kind( path ) == Node_kind::Path_expr );
    return child( path, 0 );
}

std::span<const Node_id> Ast::initialisers( Node_id id ) const
{
    assert( kind( id ) == Node_kind::Struct_literal && "initialisers are a struct literal's, not any node's" );
    return children( id ).subspan( package( id ).is_valid() ? 1 : 0 );
}

Node_id Ast::value( Node_id id ) const
{
    switch( kind( id ) )
    {
    case Node_kind::Assign_stmt:
        return child( id, 1 );
    default:
        assert( ( kind( id ) == Node_kind::Return_stmt || kind( id ) == Node_kind::Field_init ) && "this kind has no value" );
        return child( id, 0 );
    }
}

Node_id Ast::target( Node_id assign ) const
{
    assert( kind( assign ) == Node_kind::Assign_stmt );
    return child( assign, 0 );
}

Node_id Ast::written_type( Node_id id ) const
{
    assert( ( kind( id ) == Node_kind::Cast_expr || kind( id ) == Node_kind::Alloc_expr ) && "this kind names no type" );
    return child( id, 0 );
}

Node_id Ast::pointer( Node_id id ) const
{
    assert( ( kind( id ) == Node_kind::Free_expr || kind( id ) == Node_kind::Destroy_expr ) && "this kind has no pointer" );
    return child( id, 0 );
}

Node_id Ast::count( Node_id id ) const
{
    assert( ( kind( id ) == Node_kind::Alloc_expr || kind( id ) == Node_kind::Destroy_expr ) && "this kind has no count" );
    const std::span<const Node_id> all = children( id );
    return all.size() > 1 ? all[1] : Node_id {};
}

Node_id Ast::lower( Node_id range ) const
{
    assert( kind( range ) == Node_kind::Range_expr );
    return child( range, 0 );
}

Node_id Ast::upper( Node_id range ) const
{
    assert( kind( range ) == Node_kind::Range_expr );
    return child( range, 1 );
}

Node_id Ast::condition( Node_id id ) const
{
    switch( kind( id ) )
    {
    case Node_kind::For_stmt:
        return child( id, 1 );
    default:
        assert(
            ( kind( id ) == Node_kind::If_stmt || kind( id ) == Node_kind::Conditional_expr ||
              kind( id ) == Node_kind::While_stmt || kind( id ) == Node_kind::Assert_expr ) &&
            "this kind has no condition"
        );
        return child( id, 0 );
    }
}

Node_id Ast::then_branch( Node_id id ) const
{
    assert( ( kind( id ) == Node_kind::If_stmt || kind( id ) == Node_kind::Conditional_expr ) && "no branches here" );
    return child( id, 1 );
}

Node_id Ast::else_branch( Node_id id ) const
{
    assert( ( kind( id ) == Node_kind::If_stmt || kind( id ) == Node_kind::Conditional_expr ) && "no branches here" );
    return child( id, 2 );
}

Node_id Ast::expression( Node_id expr_stmt ) const
{
    assert( kind( expr_stmt ) == Node_kind::Expr_stmt );
    return child( expr_stmt, 0 );
}

Node_id Ast::init( Node_id for_stmt ) const
{
    assert( kind( for_stmt ) == Node_kind::For_stmt );
    return child( for_stmt, 0 );
}

Node_id Ast::update( Node_id for_stmt ) const
{
    assert( kind( for_stmt ) == Node_kind::For_stmt );
    return child( for_stmt, 2 );
}

Node_id Ast::scrutinee( Node_id switch_stmt ) const
{
    assert( kind( switch_stmt ) == Node_kind::Switch_stmt );
    return child( switch_stmt, 0 );
}

std::span<const Node_id> Ast::arms( Node_id switch_stmt ) const
{
    assert( kind( switch_stmt ) == Node_kind::Switch_stmt );
    return children( switch_stmt ).subspan( 1 );
}

std::span<const Node_id> Ast::labels( Node_id arm ) const
{
    assert( kind( arm ) == Node_kind::Case_arm );
    const std::span<const Node_id> all = children( arm );
    return all.first( all.size() - 1 ); // all but the body
}

Node_id Ast::variant_path( Node_id pattern ) const
{
    assert( kind( pattern ) == Node_kind::Variant_pattern );
    return child( pattern, 0 );
}

std::span<const Node_id> Ast::bindings( Node_id pattern ) const
{
    assert( kind( pattern ) == Node_kind::Variant_pattern );
    return children( pattern ).subspan( 1 );
}

bool Ast::enum_has_payload( Node_id enum_decl ) const
{
    for( const Node_id variant : variants( enum_decl ) )
    {
        if( !payload( variant ).empty() )
        {
            return true;
        }
    }

    return false;
}

Keyword Ast::parameter_mode( Node_id decl ) const
{
    const Node_id annotation = unwrap_const( declared_type( decl ) );

    return annotation.is_valid() && kind( annotation ) == Node_kind::Mode_type ? keyword( annotation ) : Keyword::Count;
}

bool Ast::is_ref_parameter( Node_id param ) const
{
    // Invalid for an `auto` local, and for a constructor or destructor, which have no return type
    // to carry a mode. kind() asserts on an invalid id rather than answering.
    const Node_id annotation = unwrap_const( declared_type( param ) );

    return annotation.is_valid() && kind( annotation ) == Node_kind::Mode_type && keyword( annotation ) == Keyword::Ref;
}

std::vector<Node_id> Ast::type_parameters( Node_id list ) const
{
    std::vector<Node_id> result;
    for( const Node_id param : type_param_decls( list ) )
    {
        if( kind( param ) != Node_kind::Type_param_decl )
        {
            continue;
        }

        result.push_back( param );
    }

    return result;
}

bool Ast::is_generic( Node_id decl ) const
{
    return type_param_list( decl ).is_valid();
}

bool Ast::is_extern( Node_id decl ) const
{
    return kind( decl ) == Node_kind::Function_decl && !body( decl ).is_valid();
}

bool Ast::has_receiver( Node_id decl ) const
{
    if( !decl.is_valid() || !is_function_like( kind( decl ) ) )
    {
        return false;
    }

    return explicit_params( decl ).size() != params( decl ).size();
}

bool Ast::is_static_method( Node_id method ) const
{
    return method.is_valid() && kind( method ) == Node_kind::Method_decl && !has_receiver( method );
}

bool Ast::is_const_binding( Node_id decl ) const
{
    // Every one of these has a declared_type: a variable's annotation, a parameter's type, a
    // function's return type - and a method's, which is why a Method_decl belongs here even
    // though the *receiver's* constness is a different question, which is_const_method asks.
    const bool declares_a_binding =
        decl.is_valid() && ( kind( decl ) == Node_kind::Var_decl || kind( decl ) == Node_kind::Param_decl ||
                             kind( decl ) == Node_kind::Function_decl || kind( decl ) == Node_kind::Method_decl );

    if( !declares_a_binding )
    {
        return false;
    }

    const Node_id annotation = declared_type( decl );
    return annotation.is_valid() && kind( annotation ) == Node_kind::Const_type;
}

bool Ast::is_const_field( Node_id decl ) const
{
    return decl.is_valid() && kind( decl ) == Node_kind::Field_decl && kind( annotation( decl ) ) == Node_kind::Const_type;
}

bool Ast::is_const_method( Node_id method ) const
{
    if( !method.is_valid() || kind( method ) != Node_kind::Method_decl )
    {
        return false;
    }

    // The trailing `const` marks the **receiver**, which the parser wraps as `const ref T` - the
    // method's return type is a different `const` entirely. So this is the ordinary const-binding
    // question, asked of parameter 0.
    return has_receiver( method ) && is_const_binding( params( method )[0] );
}

bool Ast::has_destructor( Node_id declaration ) const
{
    if( !declaration.is_valid() || !is_aggregate( kind( declaration ) ) )
    {
        return false;
    }

    for( const Node_id member : members( declaration ) )
    {
        if( kind( member ) == Node_kind::Destructor_decl )
        {
            return true;
        }
    }

    return false;
}

Node_id Ast::unwrap_const( Node_id annotation ) const
{
    return annotation.is_valid() && kind( annotation ) == Node_kind::Const_type ? inner_type( annotation ) : annotation;
}

Node_id Ast::enclosing_aggregate( Node_id member ) const
{
    if( !member.is_valid() )
    {
        return Node_id {};
    }

    for( const Node_id decl : declarations( root() ) )
    {
        if( !is_aggregate( kind( decl ) ) )
        {
            continue;
        }

        for( const Node_id candidate : members( decl ) )
        {
            if( candidate == member )
            {
                return decl;
            }
        }
    }

    return Node_id {};
}

// `from` is an aggregate rather than the function or receiver the access was written in: asking
// anything else makes every caller convert, and the conversion is where the two halves drift.
bool Ast::is_visible_from( Node_id member, Node_id from ) const
{
    if( !member.is_valid() || access( member ) != Access::Private )
    {
        return true;
    }

    return from.is_valid() && enclosing_aggregate( member ) == from;
}

std::vector<Node_id> Ast::contained_fields( Node_id declaration ) const
{
    std::vector<Node_id> fields;

    if( !declaration.is_valid() )
    {
        return fields;
    }

    if( kind( declaration ) == Node_kind::Enum_decl )
    {
        for( const Node_id variant : variants( declaration ) )
        {
            for( const Node_id field : payload( variant ) )
            {
                fields.push_back( field );
            }
        }

        return fields;
    }

    for( const Node_id member : members( declaration ) )
    {
        if( kind( member ) == Node_kind::Field_decl )
        {
            fields.push_back( member );
        }
    }

    return fields;
}

Keyword Ast::call_marker( Node_id param ) const
{
    return is_const_binding( param ) ? Keyword::Count : parameter_mode( param );
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

    REQUIRE( ast.child( a, 2 ) == leaves[2] );
    REQUIRE( ast.child( b, 0 ) == leaves[3] );
    REQUIRE( ast.child( d, 0 ) == leaves[4] );
    REQUIRE( ast.child( d, 1 ) == leaves[5] );
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

// An absent optional is an invalid id in its slot, not a shorter list: an If_stmt without `else`
// still has three children.
TEST_CASE( "ast_invalid_child_marks_an_absent_optional", "[ast]" )
{
    Ast ast;

    const Node_id cond = ast.add( Node_kind::Bool_literal, at( 0, 1 ), 1, {} );
    const Node_id then = ast.add( Node_kind::Block, at( 2, 4 ), 0, {} );
    const Node_id stmt = ast.add( Node_kind::If_stmt, at( 0, 4 ), 0, { cond, then, Node_id {} } );

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
    REQUIRE( ast.child( list, 0 ) == a );
}

// The shape the parser produces for `i32 main() { return 0; }`, built by hand.
TEST_CASE( "ast_builds_a_small_function", "[ast]" )
{
    Ast ast;

    const Node_id ret_type = ast.add( Node_kind::Named_type, at( 0, 3 ), 0, {} );
    const Node_id params   = ast.add( Node_kind::Param_list, at( 8, 10 ), 0, {} );
    const Node_id zero     = ast.add( Node_kind::Int_literal, at( 20, 21 ), 0, {} );
    const Node_id ret      = ast.add( Node_kind::Return_stmt, at( 13, 22 ), 0, { zero } );
    const Node_id body     = ast.add( Node_kind::Block, at( 11, 24 ), 0, { ret } );
    const Node_id func     = ast.add( Node_kind::Function_decl, at( 0, 24 ), 0, { ret_type, params, body, Node_id {} } );
    const Node_id file     = ast.add( Node_kind::Source_file, at( 0, 24 ), 0, { func } );

    ast.set_root( file );

    REQUIRE( ast.node_count() == 7 );
    REQUIRE( ast.kind( ast.root() ) == Node_kind::Source_file );

    const Node_id only = ast.child( ast.root(), 0 );
    REQUIRE( ast.kind( only ) == Node_kind::Function_decl );

    REQUIRE( ast.children( only ).size() == 4 );
    REQUIRE( ast.kind( ast.child( only, 0 ) ) == Node_kind::Named_type );
    REQUIRE( ast.kind( ast.child( only, 1 ) ) == Node_kind::Param_list );
    REQUIRE( ast.kind( ast.child( only, 2 ) ) == Node_kind::Block );
    REQUIRE_FALSE( ast.child( only, 3 ).is_valid() );

    const Node_id block = ast.child( only, 2 );
    REQUIRE( ast.children( block ).size() == 1 );
    REQUIRE( ast.kind( ast.child( block, 0 ) ) == Node_kind::Return_stmt );
    REQUIRE( ast.span( func ) == at( 0, 24 ) );
}

TEST_CASE( "ast_marks_a_node_broken_when_a_failure_lies_in_it", "[ast]" )
{
    SECTION( "an Error node is broken, a node with no failure in it is not" )
    {
        Ast           ast;
        const Node_id error = ast.add( Node_kind::Error, at( 0, 1 ), 0, {} );
        const Node_id fine  = ast.add( Node_kind::Int_literal, at( 2, 3 ), 0, {} );
        CHECK( ast.broken( error ) );
        CHECK_FALSE( ast.broken( fine ) );
        CHECK_FALSE( ast.broken( Node_id {} ) );
    }

    SECTION( "a failure inside the span, or at its end, breaks the node" )
    {
        Ast ast;
        ast.fail( at( 4, 5 ) );
        CHECK( ast.broken( ast.add( Node_kind::Block, at( 0, 10 ), 0, {} ) ) );

        Ast at_end;
        at_end.fail( at( 10, 10 ) ); // the gap after the node, where a missing `;` points
        CHECK( at_end.broken( at_end.add( Node_kind::Block, at( 0, 10 ), 0, {} ) ) );
    }

    SECTION( "a failure past the end, or in another file, does not" )
    {
        Ast ast;
        ast.fail( at( 11, 12 ) );
        ast.fail( Span { File_id { 1 }, 4, 5 } );
        CHECK_FALSE( ast.broken( ast.add( Node_kind::Block, at( 0, 10 ), 0, {} ) ) );
        CHECK( ast.failed_within( at( 0, 11 ) ) );
        CHECK_FALSE( ast.failed_within( at( 0, 10 ) ) );
    }

    SECTION( "a broken child breaks its parent" )
    {
        Ast           ast;
        const Node_id error = ast.add( Node_kind::Error, at( 0, 1 ), 0, {} );
        const Node_id fine  = ast.add( Node_kind::Int_literal, at( 2, 3 ), 0, {} );
        CHECK( ast.broken( ast.add( Node_kind::Block, at( 0, 3 ), 0, { error, fine } ) ) );
        CHECK_FALSE( ast.broken( ast.add( Node_kind::Block, at( 2, 3 ), 0, { fine } ) ) );
    }

    SECTION( "a failure recorded after a node is built leaves it whole" )
    {
        Ast           ast;
        const Node_id node = ast.add( Node_kind::Block, at( 0, 10 ), 0, {} );
        ast.fail( at( 4, 5 ) );
        CHECK_FALSE( ast.broken( node ) );
    }
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
