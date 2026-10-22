// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/inline_calls.h"
#include "ir/call_graph.h"
#include "ir/simplify.h"

namespace keel
{
namespace
{

// Calls and drops dominate; storage markers, gotos and returns are free.
u32 inline_cost( const Function& callee, const Inline_weights& weights )
{
    u32 cost      = 0;
    u32 call_like = 0;

    for( const Statement& statement : callee.statements )
    {
        switch( statement.kind )
        {
        case Statement_kind::Assign:
            if( statement.value.kind == Rvalue_kind::Call || statement.value.kind == Rvalue_kind::Indirect_call ||
                statement.value.kind == Rvalue_kind::Allocate || statement.value.kind == Rvalue_kind::Release )
            {
                cost += weights.call;
                call_like++;
            }
            else
            {
                cost += weights.statement;
            }
            break;
        case Statement_kind::Drop:
            cost += weights.call;
            call_like++;
            break;
        case Statement_kind::Storage_live:
        case Statement_kind::Storage_dead:
            break;
        }
    }

    for( const Block& block : callee.blocks )
    {
        if( block.terminator.kind == Terminator_kind::Branch )
        {
            cost += weights.branch;
        }
        else if( block.terminator.kind == Terminator_kind::Panic )
        {
            cost += weights.panic;
        }
    }

    // Forwarder bonus: a function that only calls another is cheaper to inline than one that does the same and more.
    if( call_like == 1 )
    {
        cost -= weights.call;
    }

    return cost;
}

// Measured against the caller's size before any splice, so one caller's limit cannot move mid-way.
u32 threshold( const u32 statements_at_start, const Function& callee, const Inline_weights& weights )
{
    if( statements_at_start >= weights.big_caller )
    {
        return weights.big_caller_threshold;
    }

    if( narrow_cast<u32>( callee.blocks.size() ) <= weights.few_blocks )
    {
        return weights.threshold + weights.threshold / 4;
    }

    return weights.threshold;
}

// Each of these is reached through `&init`, whose pointer `addressed_locals` trusts to die with its call.
bool inlinable( const Function& callee )
{
    if( callee.constructed.is_valid() || callee.out_parameters.size() > 0 || callee.owed_fields.size() > 0 )
    {
        return false;
    }

    return true;
}

// The call at statement `index` of block `at` becomes: `at` up to the call, then the parameters
// assigned from the arguments; the callee's blocks, each `return` a goto; and a continuation that
// moves the result into the call's place, then runs the rest of `at`. `at`'s old range is left
// unreferenced, for the caller to sweep.
void splice( Function& caller, Block_id at, u32 index, const Function& callee )
{
    // Copies: `blocks` and `statements` grow below.
    const Block     block = caller.blocks[at.v];
    const Statement call  = caller.statements[block.first_statement + index];

    const u32      local_base = narrow_cast<u32>( caller.locals.size() );
    const u32      block_base = narrow_cast<u32>( caller.blocks.size() );
    const u32      proj_base  = narrow_cast<u32>( caller.projections.size() );
    const u32      op_base    = narrow_cast<u32>( caller.operands.size() );
    const Local_id result { local_base };
    const Block_id continuation { block_base + narrow_cast<u32>( callee.blocks.size() ) };

    for( const Local& local : callee.locals )
    {
        caller.locals.push_back( local );
    }

    for( const Projection& projection : callee.projections )
    {
        Projection copy = projection;
        if( copy.kind == Projection_kind::Index )
        {
            copy.index.v += local_base;
        }
        caller.projections.push_back( copy );
    }

    // A global's local is invalid but its projections still move.
    const auto renumber_place = [&]( Place& place )
    {
        if( place.local.is_valid() )
        {
            place.local.v += local_base;
        }
        if( place.num_projections > 0 )
        {
            place.first_projection += proj_base;
        }
    };

    // By place, not by kind: an `Address_of` operand is a Constant holding one.
    const auto renumber = [&]( Operand& operand ) { renumber_place( operand.place ); };

    for( const Operand& operand : callee.operands )
    {
        Operand copy = operand;
        renumber( copy );
        caller.operands.push_back( copy );
    }

    for( const Block& from : callee.blocks )
    {
        Block into           = from;
        into.first_statement = narrow_cast<u32>( caller.statements.size() );

        for( u32 s = 0; s < from.statement_count; ++s )
        {
            Statement statement = callee.statements[from.first_statement + s];
            renumber_place( statement.place );
            renumber( statement.value.a );
            renumber( statement.value.b );
            if( statement.value.argument_count > 0 )
            {
                statement.value.first_argument += op_base;
            }
            caller.statements.push_back( statement );
        }

        Terminator& terminator = into.terminator;
        if( terminator.kind == Terminator_kind::Return )
        {
            terminator.kind       = Terminator_kind::Goto;
            terminator.targets[0] = continuation;
        }
        else
        {
            for( Block_id& target : terminator.targets )
            {
                if( target.is_valid() )
                {
                    target.v += block_base;
                }
            }
        }
        renumber( terminator.condition );
        renumber( terminator.message );
        renumber( terminator.at );

        caller.blocks.push_back( into );
    }

    // The continuation: the result into the call's place, then the rest of `at`.
    Block after { .first_statement = narrow_cast<u32>( caller.statements.size() ), .terminator = block.terminator };

    if( callee.returns_a_value )
    {
        const Place from { .local = result };
        caller.statements.push_back( Statement {
            .kind  = Statement_kind::Assign,
            .span  = call.span,
            .place = call.place,
            .value = use( move( from, callee.locals[0].type ) )
        } );
        caller.statements.push_back( Statement { .kind = Statement_kind::Storage_dead, .span = call.span, .place = from } );
    }

    for( u32 s = index + 1; s < block.statement_count; ++s )
    {
        const Statement statement = caller.statements[block.first_statement + s];
        caller.statements.push_back( statement );
    }

    after.statement_count = narrow_cast<u32>( caller.statements.size() ) - after.first_statement;
    caller.blocks.push_back( after );

    // `at`: its statements before the call, then the parameters' storage and values.
    const u32 head_first = narrow_cast<u32>( caller.statements.size() );

    for( u32 s = 0; s < index; ++s )
    {
        const Statement statement = caller.statements[block.first_statement + s];
        caller.statements.push_back( statement );
    }

    for( u32 p = callee.returns_a_value ? 0 : 1; p <= callee.parameter_count; ++p )
    {
        caller.statements.push_back( Statement {
            .kind = Statement_kind::Storage_live, .span = call.span, .place = Place { .local = Local_id { local_base + p } }
        } );
    }

    assert( call.value.argument_count == callee.parameter_count );
    for( u32 i = 0; i < call.value.argument_count; ++i )
    {
        const Operand argument = caller.operands[call.value.first_argument + i];
        caller.statements.push_back( Statement {
            .kind  = Statement_kind::Assign,
            .span  = call.span,
            .place = Place { .local = Local_id { local_base + 1 + i } },
            .value = use( argument ),
        } );
    }

    Block& head          = caller.blocks[at.v];
    head.first_statement = head_first;
    head.statement_count = narrow_cast<u32>( caller.statements.size() ) - head_first;
    head.terminator = Terminator { .kind = Terminator_kind::Goto, .span = call.span, .targets = { Block_id { block_base } } };

    for( const Local_id temporary : callee.one_path_temporaries )
    {
        caller.one_path_temporaries.push_back( Local_id { temporary.v + local_base } );
    }
}

} // namespace

void inline_calls(
    Function&                                   caller,
    const std::vector<std::optional<Function>>& saved,
    const Literal_pool&                         literals,
    const Inline_weights&                       weights
)
{
    const u32             statement_count = narrow_cast<u32>( caller.statements.size() );
    std::vector<Block_id> worklist( caller.blocks.size() );

    for( u32 i = 0; i < caller.blocks.size(); ++i )
    {
        worklist[i] = Block_id { i };
    }

    while( !worklist.empty() )
    {
        const Block_id at = worklist.back();
        worklist.pop_back();

        Block& block = caller.blocks[at.v];

        for( u32 s = 0; s < block.statement_count; ++s )
        {
            Statement& statement = caller.statements[block.first_statement + s];

            if( statement.kind != Statement_kind::Assign || statement.value.kind != Rvalue_kind::Call )
            {
                continue;
            }

            bool inlined = false;
            for( u32 j = 0; j < saved.size(); ++j )
            {
                if( !saved[j] )
                {
                    continue;
                }

                if( calls( statement.value, *saved[j] ) )
                {
                    if( inlinable( *saved[j] ) &&
                        inline_cost( *saved[j], weights ) <= threshold( statement_count, *saved[j], weights ) )
                    {
                        splice( caller, at, s, *saved[j] );
                        worklist.push_back( Block_id { narrow_cast<u32>( caller.blocks.size() ) - 1 } );
                        inlined = true;
                    }
                    break;
                }
            }

            if( inlined )
            {
                break;
            }
        }
    }

    remove_statements( caller, std::vector<bool>( caller.statements.size(), false ) );
    simplify( caller, literals );
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

constexpr std::string_view k_types = "class Box { public i64 value; public Box( i64 v ) { value = v; } ~Box() { } };\n"
                                     "i32 twice( i32 x ) { return x * 2; }\n"
                                     "void set( out i32 n ) { n = 9; }\n"
                                     "i32 mixed( i32 a, i32 b ) { i32 c = a * b + a; i32 d = c - b * 3; i32 e = d * d + c;"
                                     " i32 f = e / 7 + d; i32 g = f * a - e; i32 h = g * b + f * c; return h + c * d - f; }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Inlined : Compiled
{
    explicit Inlined( std::string_view source, Inline_weights weights = {} )
        : Compiled(
              std::string( source ) + std::string( k_types ), Stage::Elaborated, Build { .optimise = true, .inlining = weights }
          )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    // Direct calls left in the function under test.
    std::size_t calls() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            []( const Statement& s ) { return s.kind == Statement_kind::Assign && s.value.kind == Rvalue_kind::Call; }
        ) );
    }
};

} // namespace

TEST_CASE( "inline_calls_inlines_both_calls_in_one_block", "[opt][inline]" )
{
    const Inlined p( "i32 f( i32 a ) { return twice( a ) + twice( a + 1 ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 0 );
}

// The call inside `f` is into its own group, so the body never takes itself.
TEST_CASE( "inline_calls_keeps_a_recursive_call", "[opt][inline]" )
{
    const Inlined p( "i64 f( i64 n ) { if( n <= 1 ) { return 1; } return n * f( n - 1 ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 1 );
}

// The weights are the knobs: raise the threshold and the same call goes.
TEST_CASE( "inline_calls_keeps_a_call_over_the_threshold", "[opt][inline]" )
{
    const std::string_view source = "i32 f( i32 a ) { return mixed( a, 3 ); }\n";

    const Inlined kept( source );
    REQUIRE( kept.clean() );
    REQUIRE( kept.calls() == 1 );

    const Inlined raised( source, Inline_weights { .threshold = 100 } );
    REQUIRE( raised.clean() );
    REQUIRE( verify( raised.first() ).empty() );
    REQUIRE( raised.calls() == 0 );
}

// Both are reached through `&init`, whose pointer the passes trust to die with its call.
TEST_CASE( "inline_calls_keeps_a_call_that_initialises_through_out", "[opt][inline]" )
{
    const Inlined p( "i32 f() { i32 n = 0; set( out n ); return n; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 1 );
}

TEST_CASE( "inline_calls_keeps_a_constructor", "[opt][inline]" )
{
    const Inlined p( "i64 f() { Box b = Box( 4 ); return b.value; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
