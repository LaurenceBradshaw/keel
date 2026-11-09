// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/verify_types.h"
#include <fmt/format.h>
#include <cstdio>
#include <cstdlib>
#include "ir/place_type.h"
#include "ir/verify.h"

namespace keel
{

namespace
{

class Checking
{
public:
    Checking( const Ast& ast, Types& types, const Function& func, std::vector<std::string>& errors )
        : ast_( ast ),
          types_( types ),
          table_( types.table() ),
          func_( func ),
          errors_( errors )
    {
    }

    Type_id place( const Place& place ) const
    {
        return place_type( ast_, types_, func_, place );
    }

    void expect( Type_id got, Type_id want, const At& at, std::string_view what )
    {
        if( got != want && normal( got ) != normal( want ) )
        {
            errors_.push_back( fmt::format( "{}: {} is {}, not {}", at, what, name( got ), name( want ) ) );
        }
    }

    void rvalue( const Rvalue& value, const Function* callee, const At& at )
    {
        switch( value.kind )
        {
        case Rvalue_kind::Use:
            operand( value.a, at );
            expect( value.type, value.a.type, at, "a use" );
            return;

        case Rvalue_kind::Binary:
            operand( value.a, at( "left" ) );
            operand( value.b, at( "right" ) );
            binary( value, at );
            return;

        case Rvalue_kind::Overflows:
            operand( value.a, at( "left" ) );
            operand( value.b, at( "right" ) );
            expect( value.b.type, value.a.type, at, "the right operand" );
            expect( value.type, table_.builtin( Type_kind::Bool ), at, "an overflow test" );
            return;

        case Rvalue_kind::Unary:
            operand( value.a, at );
            expect(
                value.type, value.op == Token_kind::Bang ? table_.builtin( Type_kind::Bool ) : value.a.type, at, "a unary"
            );
            return;

        case Rvalue_kind::Call:
        case Rvalue_kind::Indirect_call:
            if( value.kind == Rvalue_kind::Indirect_call )
            {
                operand( value.a, at( "callee" ) );
            }
            for( u32 i = 0; i < value.arguments.size(); ++i )
            {
                const At argument = at( "argument", i );
                operand( value.arguments[i], argument );
                if( callee != nullptr && i < callee->parameter_count )
                {
                    expect( value.arguments[i].type, callee->locals[Local_id { i + 1 }].type, argument, "the argument" );
                }
            }
            if( callee != nullptr )
            {
                expect( value.type, callee->locals[k_return_slot].type, at, "the call" );
            }
            return;

        case Rvalue_kind::Cast:
            operand( value.a, at );
            if( ( table_.is_interface_pointer( value.type ) &&
                  !( table_.is_pointer( value.a.type ) && table_.is_interface( table_.get( value.a.type ).element ) ) ) ||
                table_.is_box( value.type ) )
            {
                errors_.push_back( fmt::format( "{}: a cast makes {} from {}", at, name( value.type ), name( value.a.type ) ) );
            }
            return;

        case Rvalue_kind::Bits:
        case Rvalue_kind::Run_data:
        case Rvalue_kind::Run_size:
        case Rvalue_kind::Release:
        case Rvalue_kind::Literal_bytes:
            operand( value.a, at );
            return;

        case Rvalue_kind::Run_of:
        case Rvalue_kind::Field_read:
            operand( value.a, at( "left" ) );
            operand( value.b, at( "right" ) );
            return;

        case Rvalue_kind::Allocate:
            if( value.a.type.is_valid() )
            {
                operand( value.a, at( "count" ) );
            }
            return;

        case Rvalue_kind::Make_interface:
            operand( value.a, at );
            if( !table_.is_interface_pointer( value.type ) || !table_.is_pointer( value.a.type ) ||
                table_.is_interface( table_.get( value.a.type ).element ) )
            {
                errors_.push_back(
                    fmt::format( "{}: an interface is made from {} as {}", at, name( value.a.type ), name( value.type ) )
                );
            }
            return;

        case Rvalue_kind::Vtable_slot:
        case Rvalue_kind::Interface_self:
            operand( value.a, at );
            if( !table_.is_interface_pointer( value.a.type ) ||
                ( value.kind == Rvalue_kind::Vtable_slot
                      ? !table_.is_function( value.type )
                      : !table_.is_pointer( value.type ) || !table_.is_void( table_.get( value.type ).element ) ) )
            {
                errors_.push_back( fmt::format( "{}: {} is read from {}", at, name( value.type ), name( value.a.type ) ) );
            }
            return;

        case Rvalue_kind::Address_of:
            address( value, at );
            return;

        case Rvalue_kind::Function_address:
        case Rvalue_kind::Field_offset:
            return;
        }
    }

    // An address is its place's pointer: an implementer's becomes an interface pointer only through Make_interface.
    void address( const Rvalue& value, const At& at )
    {
        const Type_id held = place( value.a.place );
        if( table_.is_interface_pointer( value.type ) && !table_.is_interface( held ) )
        {
            errors_.push_back( fmt::format( "{}: the address of {} is {}", at, name( held ), name( value.type ) ) );
        }
    }

    // A `void` place discards what it is given.
    void assigned( Type_id value, Type_id place, const At& at )
    {
        if( !table_.is_void( normal( place ) ) )
        {
            expect( value, place, at, "the value" );
        }
    }

    void condition( const Operand& condition, const At& at )
    {
        operand( condition, at );
        expect( condition.type, table_.builtin( Type_kind::Bool ), at, "the condition" );
    }

private:
    std::string name( Type_id type ) const
    {
        return type.is_valid() ? std::string( table_.name( type ) ) : "nothing";
    }

    // Nothing is `void`, and a pointer's `const` is the checker's alone: no backend represents it.
    Type_id normal( Type_id type ) const
    {
        if( !type.is_valid() )
        {
            return table_.builtin( Type_kind::Void );
        }

        const Type& described = table_.get( type );
        if( described.kind == Type_kind::Pointer )
        {
            return types_.table().pointer_to( normal( described.element ) );
        }
        if( described.kind == Type_kind::Many_pointer )
        {
            return types_.table().many_pointer_to( normal( described.element ) );
        }
        return type;
    }

    // An operand reads its place as the place's own type.
    void operand( const Operand& operand, const At& at )
    {
        if( operand.has_place() )
        {
            expect( operand.type, place( operand.place ), at, "the operand" );
        }
    }

    void binary( const Rvalue& value, const At& at )
    {
        // A pointer moves by a 64-bit integer.
        if( table_.is_many_pointer( value.a.type ) && !table_.is_many_pointer( value.b.type ) )
        {
            if( value.b.type != table_.integer( 64, true ) && value.b.type != table_.integer( 64, false ) )
            {
                errors_.push_back( fmt::format( "{}: a pointer moves by {}", at, name( value.b.type ) ) );
            }
            expect( value.type, value.a.type, at, "a pointer moved" );
            return;
        }

        if( is_comparison( value.op ) && value.op != Token_kind::Less_equal_greater )
        {
            expect( value.b.type, value.a.type, at, "the right operand" );
            expect( value.type, table_.builtin( Type_kind::Bool ), at, "a comparison" );
            return;
        }

        expect( value.b.type, value.a.type, at, "the right operand" );
        expect( value.type, value.a.type, at, "a binary" );
    }

    const Ast&                ast_;
    Types&                    types_;
    const Type_table&         table_;
    const Function&           func_;
    std::vector<std::string>& errors_;
};

} // namespace

Type_verifier::Type_verifier( const Ast& ast, Types& types, const Interner& interner, std::span<const Function> program )
    : ast_( ast ),
      types_( types ),
      interner_( interner ),
      program_( program )
{
    for( u32 i = 0; i < program.size(); ++i )
    {
        by_declaration_[program[i].declaration.v].push_back( i );
    }
}

const Function* Type_verifier::callee( const Rvalue& value ) const
{
    if( value.kind != Rvalue_kind::Call )
    {
        return nullptr;
    }

    const auto found = by_declaration_.find( value.callee.v );
    if( found == by_declaration_.end() )
    {
        return nullptr;
    }

    for( const u32 i : found->second )
    {
        if( names( value, program_[i] ) && !program_[i].globals.is_valid() )
        {
            return &program_[i];
        }
    }

    return nullptr;
}

std::vector<std::string> Type_verifier::check( const Function& func ) const
{
    std::vector<std::string> errors;
    Checking                 checking( ast_, types_, func, errors );

    for( u32 i = 0; i < func.statements.size(); ++i )
    {
        const Statement& statement = func.statements[i];
        if( statement.kind != Statement_kind::Assign || !statement.value.is_valid() )
        {
            continue;
        }

        const At      at { "statement", i };
        const Rvalue& value = func.rvalue( statement );

        checking.rvalue( value, callee( value ), at );
        checking.assigned( value.type, checking.place( statement.place ), at );
    }

    for( const Block_id i : func.blocks.ids() )
    {
        const Terminator& terminator = func.blocks[i].terminator;
        if( terminator.kind == Terminator_kind::Branch )
        {
            checking.condition( terminator.condition, At { "block", i.v } );
        }
    }

    return errors;
}

void Type_verifier::require( const Function& func, std::string_view pass ) const
{
    const std::vector<std::string> errors = verify( func );
    report( func, pass, errors.empty() ? check( func ) : errors );
}

void Type_verifier::require_types( const Function& func, std::string_view pass ) const
{
    report( func, pass, check( func ) );
}

void Type_verifier::report( const Function& func, std::string_view pass, const std::vector<std::string>& errors ) const
{
    if( errors.empty() )
    {
        return;
    }

    const Symbol_id name = func.declaration.is_valid() ? ast_.name( func.declaration ) : Symbol_id {};
    fmt::print(
        stderr,
        "keelc: KIR after {} is malformed, in {}:\n",
        pass,
        name.is_valid() ? interner_.text( name ) : std::string_view( "a function with no name" )
    );
    for( const std::string& error : errors )
    {
        fmt::print( stderr, "    {}\n", error );
    }
    std::abort();
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <fmt/ranges.h>
#include <catch2/catch_test_macros.hpp>
#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

// As lowered, so the cases see what lowering itself writes.
struct Verified : Compiled
{
    explicit Verified( std::string_view source )
        : Compiled( std::string( source ) + "\ni32 main() { return 0; }", Stage::Lowered )
    {
    }

    Function& named( std::string_view name )
    {
        const Symbol_id symbol = interner.find( name );
        for( Function& function : all )
        {
            if( function.declaration.is_valid() && ast.name( function.declaration ) == symbol )
            {
                return function;
            }
        }
        FAIL( "no function " << name );
        return all.front();
    }

    // The first assignment whose rvalue is of `kind`.
    Rvalue& first( Function& function, Rvalue_kind kind )
    {
        for( const Statement& statement : function.statements )
        {
            if( statement.value.is_valid() && function.rvalue( statement ).kind == kind )
            {
                return function.rvalue( statement );
            }
        }
        FAIL( "no rvalue of that kind" );
        return function.rvalues.front();
    }

    std::vector<std::string> check( std::string_view name )
    {
        const Type_verifier verifier( ast, types, interner, all );
        return verifier.check( named( name ) );
    }

    bool mentions( std::string_view name, std::string_view needle )
    {
        const std::vector<std::string> errors = check( name );
        INFO( fmt::format( "{}", fmt::join( errors, "\n" ) ) );
        return std::ranges::any_of( errors, [&]( const std::string& e ) { return e.find( needle ) != std::string::npos; } );
    }
};

} // namespace

TEST_CASE( "verify_types_accepts_what_lowering_writes", "[ir][verify]" )
{
    Verified p( "i32 g( i64 x ) { return 0; }\n"
                "i64 f( i32 n, u8 k, i32[*] q, f64 h ) {\n"
                "    i64 w = n; u32 one = 1; u32 s = one << k;\n"
                "    unsafe { i32 e = q[n]; const i32* d = &q[0]; }\n"
                "    g( n ); if( w > h ) { return n; } return s; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const std::vector<std::string> errors = p.check( "f" );
    INFO( fmt::format( "{}", fmt::join( errors, "\n" ) ) );
    REQUIRE( errors.empty() );
}

TEST_CASE( "verify_types_rejects_what_disagrees", "[ir][verify]" )
{
    SECTION( "a value of another type than its place" )
    {
        Verified p( "i64 f( i32 n ) { return n; }" );
        Rvalue&  cast = p.first( p.named( "f" ), Rvalue_kind::Cast );
        cast          = use( cast.a );

        REQUIRE( p.mentions( "f", "the value is i32, not i64" ) );
    }

    SECTION( "an operand of another type than its place" )
    {
        Verified p( "i64 f( i64 a, i64 b ) { return a + b; }" );
        p.first( p.named( "f" ), Rvalue_kind::Binary ).b.type = p.types.table().integer( 32, true );

        REQUIRE( p.mentions( "f", "the operand is i32, not i64" ) );
        REQUIRE( p.mentions( "f", "the right operand is i32, not i64" ) );
    }

    SECTION( "an argument of another type than its parameter" )
    {
        Verified  p( "void g( i64 x ) { }\nvoid f( i32 n ) { g( n ); }" );
        Function& f       = p.named( "f" );
        Rvalue&   call    = p.first( f, Rvalue_kind::Call );
        call.arguments[0] = copy( Place { .local = Local_id { 1 } }, f.locals[Local_id { 1 }].type );

        REQUIRE( p.mentions( "f", "the argument is i32, not i64" ) );
    }

    SECTION( "a pointer moved by a narrow offset" )
    {
        Verified p( "i32 f( i32[*] q, i64 n ) { unsafe { return q[n]; } }" );
        Rvalue&  moved = p.first( p.named( "f" ), Rvalue_kind::Binary );
        moved.b        = constant( p.literals.add_integer( 0 ), p.types.table().integer( 32, true ) );

        REQUIRE( p.mentions( "f", "a pointer moves by i32" ) );
    }

    SECTION( "a branch on something other than a bool" )
    {
        Verified p( "i32 f( bool c ) { if( c ) { return 1; } return 0; }" );
        for( Block& block : p.named( "f" ).blocks )
        {
            if( block.terminator.kind == Terminator_kind::Branch )
            {
                block.terminator.condition = constant( p.literals.add_integer( 0 ), p.types.table().integer( 32, true ) );
            }
        }

        REQUIRE( p.mentions( "f", "the condition is i32, not bool" ) );
    }
}

} // namespace keel
#endif
