// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/description.h"
#include <fmt/format.h>
#include <algorithm>
#include <cassert>
#include "ast/ast.h"
#include "sema/type_checker.h"

namespace keel
{
namespace
{
// Empty for what has no name: an `implements` block, an anonymous capture.
std::string name_of( const Ast& ast, const Interner& interner, Node_id decl )
{
    const Symbol_id name = ast.name( decl );
    return name.is_valid() ? std::string( interner.text( name ) ) : std::string {};
}

// What a call site writes for each parameter, which tells two overloads apart; and each as it travels.
void describe_parameters( const Ast& ast, const Types& types, Node_id decl, Described& described )
{
    for( const Node_id param : ast.params( decl ) )
    {
        const Keyword mode   = ast.call_marker( param );
        const char    marker = mode == Keyword::Ref ? 'R' : mode == Keyword::Move ? 'M' : mode == Keyword::Out ? 'O' : '\0';

        described.parameters.push_back( Mangled_parameter { .type = types.type_of( param ), .marker = marker } );
        described.passed.push_back( binding_type( ast, types, param ) );
    }
}

std::vector<Type_id> type_parameters( const Ast& ast, const Types& types, Node_id decl )
{
    std::vector<Type_id> parameters;
    for( const Node_id parameter : ast.type_parameters( ast.type_param_list( decl ) ) )
    {
        parameters.push_back( types.type_of( parameter ) );
    }
    return parameters;
}

void describe_member( const Ast& ast, const Types& types, const Interner& interner, Node_id decl, Described& described )
{
    const Node_id aggregate = ast.enclosing_aggregate( decl );
    if( !aggregate.is_valid() )
    {
        return;
    }

    described.aggregate       = aggregate;
    described.aggregate_name  = name_of( ast, interner, aggregate );
    described.aggregate_arity = narrow_cast<u32>( ast.type_parameters( ast.type_param_list( aggregate ) ).size() );

    if( ast.kind( aggregate ) == Node_kind::Implements_decl )
    {
        described.implementer = types.type_of( aggregate );
        described.conformed   = types.type_of( ast.implemented( aggregate )[0] );
    }
}

Described describe_function( const Ast& ast, const Types& types, const Interner& interner, Node_id decl )
{
    const Node_kind kind = ast.kind( decl );
    Described       described {
              .kind = kind == Node_kind::Method_decl        ? Declared::Method
                      : kind == Node_kind::Constructor_decl ? Declared::Constructor
                      : kind == Node_kind::Destructor_decl  ? Declared::Destructor
                      : kind == Node_kind::Lambda_expr      ? Declared::Lambda
                                                            : Declared::Function,
              .span = ast.span( decl ),
    };

    described.name =
        kind == Node_kind::Lambda_expr ? fmt::format( "0lambda{}", ast.span( decl ).start ) : name_of( ast, interner, decl );
    described.is_extern       = kind == Node_kind::Function_decl && ast.is_extern( decl );
    described.receiver        = kind != Node_kind::Lambda_expr && ast.has_receiver( decl );
    described.requirement     = kind == Node_kind::Method_decl && ast.is_requirement( decl );
    described.type            = binding_type( ast, types, decl );
    described.type_parameters = type_parameters( ast, types, decl );
    describe_parameters( ast, types, decl, described );
    describe_member( ast, types, interner, decl, described );
    return described;
}

// A field's symbol: a payload field's carries its variant, since two variants may share a name.
std::string field_symbol( const Ast& ast, const Interner& interner, Node_id decl )
{
    const Node_id variant = ast.variant_of( decl );
    return variant.is_valid() ? mangle_payload_field( interner.text( ast.name( variant ) ), interner.text( ast.name( decl ) ) )
                              : mangle_field( interner.text( ast.name( decl ) ) );
}
} // namespace

const Described& Description::operator[]( Node_id declaration ) const
{
    const auto found = declarations_.find( declaration.v );
    assert( found != declarations_.end() && "the description holds every declaration" );
    return found->second;
}

std::span<const Held_field> Description::fields( Type_id aggregate ) const
{
    const auto found = instances_.find( aggregate.v );
    assert( found != instances_.end() && "the description holds every aggregate instance" );
    return found->second.fields;
}

const Held_field* Description::field_named( Type_id aggregate, std::string_view name ) const
{
    for( const Held_field& field : fields( aggregate ) )
    {
        if( ( *this )[field.declaration].name == name )
        {
            return &field;
        }
    }
    return nullptr;
}

bool Description::owns( const Type_table& table, Type_id type ) const
{
    if( !type.is_valid() || table.is_error( type ) )
    {
        return false;
    }

    // Some implementer may own, and only the vtable knows which.
    if( table.is_parameter( type ) || table.is_interface( type ) )
    {
        return true;
    }

    if( table.is_array( type ) )
    {
        return owns( table, table.get( type ).element );
    }

    if( table.is_union( type ) )
    {
        for( const Type_id member : table.get( type ).arguments )
        {
            if( owns( table, member ) )
            {
                return true;
            }
        }
        return false;
    }

    const auto found = instances_.find( type.v );
    return ( table.is_struct( type ) || table.is_enum( type ) ) && found != instances_.end() && found->second.owns;
}

Type_id Description::type_of( const Type_table& table, const Function& function, const Place& place ) const
{
    Type_id type = place.is_global() ? ( *this )[place.global].type : function.locals[place.local].type;

    for( const Projection& projection : function.projections_of( place ) )
    {
        if( projection.kind == Projection_kind::Field )
        {
            const std::span<const Held_field> held  = fields( type );
            const auto                        found = std::ranges::find( held, projection.field, &Held_field::declaration );
            assert( found != held.end() && "a field of the place's aggregate" );
            type = found->type;
        }
        else
        {
            type = projection.kind == Projection_kind::Member ? projection.member : table.get( type ).element;
        }
    }

    return type;
}

std::span<const Node_id> Description::globals() const
{
    return globals_;
}

std::span<const Node_id> Description::externs() const
{
    return externs_;
}

Description describe( const Ast& ast, Types& types, const Interner& interner )
{
    Description description;

    for( u32 n = 0; n < ast.node_count(); ++n )
    {
        const Node_id   decl { n };
        const Node_kind kind = ast.kind( decl );

        if( is_function_like( kind ) || kind == Node_kind::Lambda_expr )
        {
            description.declarations_.emplace( n, describe_function( ast, types, interner, decl ) );
        }
        else if( kind == Node_kind::Field_decl || kind == Node_kind::Capture_decl )
        {
            description.declarations_.emplace(
                n,
                Described {
                    .kind   = Declared::Field,
                    .name   = name_of( ast, interner, decl ),
                    .span   = ast.span( decl ),
                    .type   = types.type_of( decl ),
                    .symbol = field_symbol( ast, interner, decl ),
                }
            );
        }
        else if( is_aggregate( kind ) || kind == Node_kind::Enum_decl || kind == Node_kind::Interface_decl )
        {
            Described described {
                .kind = kind == Node_kind::Enum_decl        ? Declared::Enum
                        : kind == Node_kind::Interface_decl ? Declared::Interface
                                                            : Declared::Aggregate,
                .name = name_of( ast, interner, decl ),
                .span = ast.span( decl ),
            };
            described.type_parameters = type_parameters( ast, types, decl );

            if( kind == Node_kind::Enum_decl )
            {
                described.payload = ast.enum_has_payload( decl );
            }
            else if( kind == Node_kind::Interface_decl )
            {
                const std::span<const Node_id> members = ast.members( decl );
                described.members.assign( members.begin(), members.end() );
            }
            else
            {
                described.destructor = ast.destructor( decl );
            }

            description.declarations_.emplace( n, std::move( described ) );
        }
    }

    // Root globals and statics alike, so a static reads `aggregate` as a member does.
    const auto global = [&]( Node_id decl )
    {
        Described described {
            .kind = Declared::Global,
            .name = name_of( ast, interner, decl ),
            .span = ast.span( decl ),
            .type = types.type_of( decl ),
        };
        describe_member( ast, types, interner, decl, described );
        description.declarations_.emplace( decl.v, std::move( described ) );
    };

    for( const Node_id child : ast.declarations( ast.root() ) )
    {
        if( ast.kind( child ) == Node_kind::Var_decl )
        {
            global( child );
            description.globals_.push_back( child );
        }
        else if( is_aggregate( ast.kind( child ) ) )
        {
            for( const Node_id member : ast.members( child ) )
            {
                if( ast.kind( member ) == Node_kind::Var_decl )
                {
                    global( member );
                    if( !ast.is_generic( child ) )
                    {
                        description.globals_.push_back( member );
                    }
                }
            }
        }
        else if( ast.is_extern( child ) )
        {
            description.externs_.push_back( child );
        }
    }

    Type_table& table    = types.table();
    const auto  instance = [&]( Type_id type )
    {
        const Node_id        declaration = table.get( type ).declaration;
        std::vector<Node_id> held;
        if( table.is_closure( type ) )
        {
            const std::span<const Node_id> captures = ast.captures( declaration );
            held.assign( captures.begin(), captures.end() );
        }
        else if( declaration.is_valid() )
        {
            held = ast.contained_fields( declaration );
        }

        Description::Instance described { .owns = instance_owns( ast, table, type, types.recorded() ) };
        for( const Node_id field : held )
        {
            described.fields.push_back( { field, field_type( ast, table, type, field, types.recorded() ) } );
        }
        description.instances_.emplace( type.v, std::move( described ) );
    };

    // Until nothing new appears, as describing one instance can intern another through a field.
    for( bool grew = true; grew; )
    {
        grew = false;
        for( const std::vector<Type_id>& kind : { table.composite_types(), table.closure_types() } )
        {
            for( const Type_id type : kind )
            {
                if( !table.mentions_parameter( type ) && !description.instances_.contains( type.v ) )
                {
                    instance( type );
                    grew = true;
                }
            }
        }
    }

    return description;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include "check/pipeline_test_support.h"

namespace keel
{

// A generic's field at each instance, an enum's payload, and what owns: each told without the AST.
TEST_CASE( "describe_gives_each_instance_its_fields", "[codegen][description]" )
{
    Compiled p(
        "class Box<T> { public T held; public i32 count; public Box( move T h, i32 c ) { held = move h; count = c; } };\n"
        "enum Shape { Dot, Square( i32 side ) };\n"
        "class Buf { u8[*] b; public Buf() { unsafe { b = alloc<u8>( 1 ); } } ~Buf() { unsafe { free( b ); } } };\n"
        "i32 main() { Box<i64> a = Box<i64>( 1, 2 ); Box<Buf> b = Box<Buf>( Buf(), 3 ); Shape s = Shape::Square( 2 ); "
        "switch( s ) { case Shape::Dot: return 1; case Shape::Square( side ): return a.count + b.count + side - 7; } }\n",
        Stage::Elaborated
    );
    REQUIRE( p.clean() );

    const Description description = describe( p.ast, p.types, p.interner );
    Type_table&       table       = p.types.table();

    const auto instance = [&]( std::string_view name )
    {
        for( const Type_id type : table.composite_types() )
        {
            if( table.name( type ) == name )
            {
                return type;
            }
        }
        return Type_id {};
    };

    const Type_id wide = instance( "Box<i64>" );
    REQUIRE( wide.is_valid() );
    REQUIRE( description.fields( wide ).size() == 2 );
    REQUIRE( description.fields( wide )[0].type == table.integer( 64, true ) );
    REQUIRE( description[description.fields( wide )[1].declaration].symbol == "kl_count" );
    REQUIRE( description.field_named( wide, "held" ) == &description.fields( wide )[0] );
    REQUIRE_FALSE( description.owns( table, wide ) );

    const Type_id owning = instance( "Box<Buf>" );
    REQUIRE( owning.is_valid() );
    REQUIRE( description.owns( table, owning ) );

    const Type_id shape = instance( "Shape" );
    REQUIRE( shape.is_valid() );
    REQUIRE( description[table.get( shape ).declaration].payload );
    REQUIRE( description.fields( shape ).size() == 1 );
    REQUIRE( description[description.fields( shape )[0].declaration].symbol == "kl_6Square_side" );
}

} // namespace keel
#endif
