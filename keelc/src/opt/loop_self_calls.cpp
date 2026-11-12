// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/loop_self_calls.h"

#include <algorithm>
#include <optional>

namespace keel
{
namespace
{
// A local's own storage is borrowed, so a deeper call may still read the frame a loop would reuse.
bool borrows_a_local( const Function& func )
{
    return std::ranges::any_of(
        func.statements,
        [&]( const Statement& statement )
        {
            const Rvalue& value = func.rvalue( statement );
            return statement.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Address_of &&
                   !value.a.place.is_global() && !has_deref_projection( func, value.a.place );
        }
    );
}

// The statement of `block` calling `func` whose result it returns, followed only by markers.
std::optional<u32> self_call( const Function& func, const Block& block )
{
    if( block.terminator.kind != Terminator_kind::Return )
    {
        return std::nullopt;
    }

    for( u32 s = block.first_statement + block.statement_count; s-- > block.first_statement; )
    {
        const Statement& statement = func.statements[s];
        if( statement.kind == Statement_kind::Storage_dead )
        {
            continue;
        }

        const Rvalue& value = func.rvalue( statement );
        const bool    returned =
            statement.place.is_whole_local() && ( !func.returns_a_value || statement.place.local == k_return_slot );
        if( statement.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Call && names( value, func ) && returned )
        {
            return s;
        }
        return std::nullopt;
    }

    return std::nullopt;
}

bool is_parameter( const Function& func, Local_id local )
{
    return local.v >= 1 && local.v <= func.parameter_count;
}
} // namespace

void loop_self_calls( Function& func )
{
    // A constructor's receiver and an `out` parameter arrive uninitialised, which a second turn would not.
    if( func.constructed.is_valid() || !func.out_parameters.empty() || borrows_a_local( func ) )
    {
        return;
    }

    std::vector<std::optional<u32>> calls;
    for( const Block& block : func.blocks )
    {
        calls.push_back( self_call( func, block ) );
    }
    if( std::ranges::none_of( calls, []( const std::optional<u32>& call ) { return call.has_value(); } ) )
    {
        return;
    }

    Statements statements;
    statements.reserve( func.statements.size() + func.parameter_count * 2 );
    for( const Block_id b : func.blocks.ids() )
    {
        Block&    block = func.blocks[b];
        const u32 first = block.first_statement;
        const u32 end   = first + block.statement_count;

        block.first_statement = narrow_cast<u32>( statements.size() );
        for( u32 s = first; s < ( calls[b.v] ? *calls[b.v] : end ); ++s )
        {
            statements.push_back( std::move( func.statements[s] ) );
        }

        if( calls[b.v] )
        {
            const Span                 span      = func.statements[*calls[b.v]].span;
            const std::vector<Operand> arguments = func.rvalue( func.statements[*calls[b.v]] ).arguments;

            // Every argument read before any parameter is written, as an argument may read one.
            std::vector<Local_id> passed;
            for( u32 i = 0; i < arguments.size(); ++i )
            {
                const Local& parameter = func.locals[Local_id { i + 1 }];
                passed.push_back( func.locals.push_back( Local { .type = parameter.type, .span = parameter.span } ) );
                statements.push_back( Statement {
                    .kind  = Statement_kind::Assign,
                    .span  = span,
                    .place = Place { .local = passed.back() },
                    .value = func.add_rvalue( use( arguments[i] ) ),
                } );
            }

            // A parameter's storage lives on into the next turn.
            for( u32 s = *calls[b.v] + 1; s < end; ++s )
            {
                if( !is_parameter( func, func.statements[s].place.local ) )
                {
                    statements.push_back( std::move( func.statements[s] ) );
                }
            }

            for( u32 i = 0; i < passed.size(); ++i )
            {
                const Local_id parameter { i + 1 };
                statements.push_back( Statement {
                    .kind  = Statement_kind::Assign,
                    .span  = span,
                    .place = Place { .local = parameter },
                    .value = func.add_rvalue( use( move( Place { .local = passed[i] }, func.locals[parameter].type ) ) ),
                } );
            }

            block.terminator =
                Terminator { .kind = Terminator_kind::Goto, .span = block.terminator.span, .targets = { k_entry_block } };
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

namespace keel
{
namespace
{
constexpr std::string_view k_main =
    "i64 dropped;\n"
    "class Box { public Box( i64 v ) { value = v; } ~Box() { dropped += 1; } public i64 value; };\n"
    "i32 main() { return 0; }\n";

// Optimised, as a build at `level` lowers it. The function under test is always the first.
struct Looped : Compiled
{
    explicit Looped( std::string_view source, u32 level = 2 )
        : Compiled( std::string( source ) + std::string( k_main ), Stage::Elaborated, Build { .level = level } )
    {
    }

    bool calls_itself() const
    {
        const Function& func = functions.front();
        return std::ranges::any_of(
            func.statements,
            [&]( const Statement& s ) {
                return s.kind == Statement_kind::Assign && func.rvalue( s ).kind == Rvalue_kind::Call &&
                       names( func.rvalue( s ), func );
            }
        );
    }
};
} // namespace

TEST_CASE( "loop_self_calls_turns_a_returned_self_call_into_a_jump", "[opt][loop_self_calls]" )
{
    const Looped p( "i64 acc( i64 n, i64 t ) { if( n == 0 ) { return t; } return acc( n - 1, t + n ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.functions.front() ).empty() );
    REQUIRE( !p.calls_itself() );
}

TEST_CASE( "loop_self_calls_passes_an_owning_parameter_on", "[opt][loop_self_calls]" )
{
    const Looped p( "i64 last( move Box b, i64 n ) { if( n == 0 ) { return b.value; } return last( move b, n - 1 ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.functions.front() ).empty() );
    REQUIRE( !p.calls_itself() );
}

TEST_CASE( "loop_self_calls_keeps_a_call_a_drop_follows", "[opt][loop_self_calls]" )
{
    const Looped p( "i64 f( move Box b, i64 n ) { if( n == 0 ) { return 0; } return f( Box( n ), n - 1 ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( p.calls_itself() );
}

TEST_CASE( "loop_self_calls_keeps_a_call_a_local_is_borrowed_for", "[opt][loop_self_calls]" )
{
    const Looped p( "i64 f( i64 n, i64* p ) { if( n == 0 ) { return *p; } i64 x = n; return f( n - 1, &x ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( p.calls_itself() );
}

TEST_CASE( "loop_self_calls_keeps_every_frame_at_O1", "[opt][loop_self_calls]" )
{
    const Looped p( "i64 acc( i64 n, i64 t ) { if( n == 0 ) { return t; } return acc( n - 1, t + n ); }\n", 1 );
    REQUIRE( p.clean() );
    REQUIRE( p.calls_itself() );
}
} // namespace keel
#endif
