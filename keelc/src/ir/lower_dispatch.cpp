// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/lower_dispatch.h"
#include <algorithm>
#include <utility>

namespace keel
{

void lower_dispatch( Function& func, const Ast& ast, Type_table& table )
{
    const auto dispatched = [&]( const Statement& statement )
    {
        const Rvalue& value = std::as_const( func ).rvalue( statement );
        return statement.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Call && value.op == Token_kind::Unknown &&
               ast.is_requirement( value.callee );
    };

    if( std::ranges::none_of( func.statements, dispatched ) )
    {
        return;
    }

    const Type_id any = table.pointer_to( table.builtin( Type_kind::Void ) );

    std::vector<Statement> statements;
    statements.reserve( func.statements.size() );

    for( Block& block : func.blocks )
    {
        const u32 first       = block.first_statement;
        const u32 count       = block.statement_count;
        block.first_statement = narrow_cast<u32>( statements.size() );

        for( u32 s = first; s < first + count; ++s )
        {
            Statement statement = std::move( func.statements[s] );

            if( !dispatched( statement ) )
            {
                statements.push_back( std::move( statement ) );
                continue;
            }

            Rvalue  call      = func.rvalue( statement );
            Operand reference = call.arguments[0];
            reference.kind    = Operand_kind::Copy;

            // Every argument by value, as the slot takes it: a borrow is already a pointer here.
            std::vector<Parameter> parameters { { any, Param_mode::Move } };
            for( std::size_t i = 1; i < call.arguments.size(); ++i )
            {
                parameters.push_back( { call.arguments[i].type, Param_mode::Move } );
            }
            const Type_id slot_type = table.function( call.type, parameters );

            const auto temporary = [&]( Type_id type )
            {
                func.locals.push_back( Local { .type = type, .span = statement.span } );
                return Place { .local = Local_id { narrow_cast<u32>( func.locals.size() - 1 ) } };
            };
            const Place slot   = temporary( slot_type );
            const Place object = temporary( any );

            Statement read_slot = func.copy_of( statement, vtable_slot( reference, call.callee, slot_type ) );
            read_slot.place     = slot;
            Statement read_self = func.copy_of( statement, interface_self( reference, any ) );
            read_self.place     = object;

            call.arguments[0]        = copy( object, any );
            func.rvalue( statement ) = indirect_call( copy( slot, slot_type ), std::move( call.arguments ), call.type );

            statements.push_back( std::move( read_slot ) );
            statements.push_back( std::move( read_self ) );
            statements.push_back( std::move( statement ) );
        }

        block.statement_count = narrow_cast<u32>( statements.size() ) - block.first_statement;
    }

    func.statements = std::move( statements );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include "check/pipeline_test_support.h"
#include "ir/verify.h"
#include "ir/verify_types.h"

namespace keel
{

// `s.area( 2 )` reads the slot and the object from `s`, and calls the one with the other and `2`.
TEST_CASE( "lower_dispatch_calls_a_requirement_through_its_vtable_slot", "[ir][dispatch]" )
{
    Compiled p(
        "interface Shape { i32 area( i32 scale ) const; };\n"
        "struct Sq implements Shape { i32 side; i32 area( i32 scale ) const { return side * scale; } };\n"
        "i32 f( const ref Shape s ) { return s.area( 2 ); }\n"
        "i32 main() { Sq q = Sq { 1 }; return f( q ) - 2; }\n",
        Stage::Elaborated
    );
    REQUIRE( p.clean() );

    const auto found = std::ranges::find_if(
        p.functions,
        [&]( const Function& function )
        { return function.declaration.is_valid() && p.interner.text( p.ast.name( function.declaration ) ) == "f"; }
    );
    REQUIRE( found != p.functions.end() );
    const Function& f = *found;
    REQUIRE( verify( f ).empty() );
    REQUIRE( Type_verifier( p.ast, p.types, p.interner, p.all ).check( f ).empty() );

    const auto count = [&]( Rvalue_kind kind )
    {
        return std::ranges::count_if(
            f.statements, [&]( const Statement& s ) { return s.kind == Statement_kind::Assign && f.rvalue( s ).kind == kind; }
        );
    };
    REQUIRE( count( Rvalue_kind::Call ) == 0 );
    REQUIRE( count( Rvalue_kind::Vtable_slot ) == 1 );
    REQUIRE( count( Rvalue_kind::Interface_self ) == 1 );
    REQUIRE( count( Rvalue_kind::Indirect_call ) == 1 );

    const auto called = std::ranges::find_if(
        f.statements,
        [&]( const Statement& s )
        { return s.kind == Statement_kind::Assign && f.rvalue( s ).kind == Rvalue_kind::Indirect_call; }
    );
    REQUIRE( f.rvalue( *called ).arguments.size() == 2 );
}

} // namespace keel
#endif
