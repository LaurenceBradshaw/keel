// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/inline_calls.h"
#include "ir/call_graph.h"
#include "ir/simplify.h"

namespace keel
{
namespace
{

// What a pick chose: no callee leaves the statement; a receiver stands in for the callee's `this`.
struct Site
{
    const Function*      callee = nullptr;
    std::optional<Place> receiver;
};

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

// An `out` parameter is reached through `&init`, whose pointer `addressed_locals` trusts to die with its call.
bool inlinable( const Function& callee )
{
    if( callee.out_parameters.size() > 0 )
    {
        return false;
    }

    return true;
}

// The offset in `block`, before `index`, of the `&init` that last wrote `pointer`'s local; none if
// another write comes between, or the `&init` is in an earlier block.
std::optional<u32> initialised( const Function& caller, const Block& block, u32 index, const Operand& pointer )
{
    if( pointer.kind != Operand_kind::Copy || pointer.place.is_global() || pointer.place.num_projections != 0 )
    {
        return std::nullopt;
    }

    for( u32 s = index; s-- > 0; )
    {
        const Statement& statement = caller.statements[block.first_statement + s];

        if( statement.kind != Statement_kind::Assign || statement.place.is_global() || statement.place.num_projections != 0 ||
            statement.place.local != pointer.place.local )
        {
            continue;
        }

        if( statement.value.kind == Rvalue_kind::Address_of && statement.value.address_purpose == Address_purpose::Initialise )
        {
            return s;
        }

        return std::nullopt;
    }

    return std::nullopt;
}

// The call at statement `index` of block `at` becomes: `at` up to the call, then the parameters
// assigned from the arguments; the callee's blocks, each `return` a goto; and a continuation that
// moves the result into the call's place, then runs the rest of `at`. `at`'s old range is left
// unreferenced, for the caller to sweep. With a `receiver`, `(*this)` is that place and `this` gets
// no local; a constructor's `&init` moves to the continuation, so the body writes no borrowed local.
void splice( Function& caller, Block_id at, u32 index, const Function& callee, std::optional<Place> receiver )
{
    // Copies: `blocks` and `statements` grow below.
    const Block              block = caller.blocks[at.v];
    const Statement          call  = caller.statements[block.first_statement + index];
    const std::optional<u32> built = receiver && call.kind == Statement_kind::Assign
                                         ? initialised( caller, block, index, caller.operands[call.value.first_argument] )
                                         : std::nullopt;

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
        if( receiver && !place.is_global() && place.local == Local_id { 1 } )
        {
            assert( place.num_projections > 0 );
            const u32 rest  = place.num_projections - 1;
            const u32 first = narrow_cast<u32>( caller.projections.size() );
            for( u32 k = 0; k < receiver->num_projections; k++ )
            {
                const Projection projection = caller.projections[receiver->first_projection + k];
                caller.projections.push_back( projection );
            }
            for( u32 k = 0; k < rest; k++ )
            {
                const Projection projection = caller.projections[proj_base + place.first_projection + 1 + k];
                caller.projections.push_back( projection );
            }

            place                  = *receiver;
            place.first_projection = first;
            place.num_projections  = receiver->num_projections + rest;
            return;
        }

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
            if( statement.drop_flag.is_valid() )
            {
                statement.drop_flag.v += local_base;
            }
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

    if( built )
    {
        const Statement statement = caller.statements[block.first_statement + *built];
        caller.statements.push_back( statement );
    }

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
        if( built && s == *built )
        {
            continue;
        }

        const Statement statement = caller.statements[block.first_statement + s];
        caller.statements.push_back( statement );
    }

    for( u32 p = callee.returns_a_value ? 0 : 1; p <= callee.parameter_count; ++p )
    {
        if( p == 1 && receiver )
        {
            continue;
        }

        caller.statements.push_back( Statement {
            .kind = Statement_kind::Storage_live, .span = call.span, .place = Place { .local = Local_id { local_base + p } }
        } );
    }

    assert( call.kind == Statement_kind::Drop || call.value.argument_count == callee.parameter_count );
    for( u32 i = 0; i < call.value.argument_count; ++i )
    {
        if( i == 0 && receiver )
        {
            continue;
        }

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

// Whether the receiver is only ever dereferenced, so the place it points at can stand in for it.
bool replaceable_receiver( const Function& callee )
{
    const auto ok = [&]( const Place& place )
    {
        if( !place.local.is_valid() || place.local != Local_id { 1 } ||
            ( place.num_projections > 0 && callee.projections[place.first_projection].kind == Projection_kind::Deref ) )
        {
            return true;
        }
        return false;
    };

    for( const Statement& statement : callee.statements )
    {
        if( !ok( statement.place ) )
        {
            return false;
        }

        bool all_ok = true;
        for_each_operand(
            callee,
            statement.value,
            [&]( const Operand& operand )
            {
                if( !ok( operand.place ) )
                {
                    all_ok = false;
                }
            }
        );

        if( !all_ok )
        {
            return false;
        }
    }

    for( const Block& block : callee.blocks )
    {
        if( !ok( block.terminator.condition.place ) || !ok( block.terminator.message.place ) ||
            !ok( block.terminator.at.place ) )
        {
            return false;
        }
    }

    return true;
}

// Splices whatever `pick` chooses, then rescans the continuation for more.
template <typename Pick>
void inline_each( Function& caller, const Literal_pool& literals, Pick pick )
{
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
            const Site site = pick( block, s );
            if( site.callee )
            {
                splice( caller, at, s, *site.callee, site.receiver );
                worklist.push_back( Block_id { narrow_cast<u32>( caller.blocks.size() ) - 1 } );
                break;
            }
        }
    }

    remove_statements( caller, std::vector<bool>( caller.statements.size(), false ) );
    simplify( caller, literals );
}

} // namespace

void inline_calls(
    Function&                                   caller,
    const std::vector<std::optional<Function>>& saved,
    const Literal_pool&                         literals,
    const Inline_weights&                       weights
)
{
    const u32 statement_count = narrow_cast<u32>( caller.statements.size() );
    inline_each(
        caller,
        literals,
        [&]( const Block& block, u32 s ) -> Site
        {
            const Statement& statement = caller.statements[block.first_statement + s];
            if( statement.kind != Statement_kind::Assign || statement.value.kind != Rvalue_kind::Call )
            {
                return {};
            }

            for( u32 j = 0; j < saved.size(); ++j )
            {
                if( !saved[j] )
                {
                    continue;
                }

                if( calls( statement, *saved[j] ) )
                {
                    const Function&      callee = *saved[j];
                    std::optional<Place> receiver;
                    bool                 fits =
                        inlinable( callee ) && inline_cost( callee, weights ) <= threshold( statement_count, callee, weights );
                    if( fits && callee.constructed.is_valid() )
                    {
                        assert( callee.constructed == Local_id { 1 } );
                        const std::optional<u32> built =
                            initialised( caller, block, s, caller.operands[statement.value.first_argument] );
                        if( built )
                        {
                            receiver = caller.statements[block.first_statement + *built].value.a.place;
                        }
                        fits = receiver.has_value() && replaceable_receiver( callee );
                    }
                    if( fits )
                    {
                        return Site { .callee = &callee, .receiver = receiver };
                    }
                    else
                    {
                        return {};
                    }
                }
            }

            return {};
        }
    );
}

void inline_drops(
    Function&                    caller,
    const std::vector<Function>& functions,
    const std::vector<bool>&     finished,
    const Literal_pool&          literals,
    const Inline_weights&        weights
)
{
    const u32 statement_count = narrow_cast<u32>( caller.statements.size() );
    inline_each(
        caller,
        literals,
        [&]( const Block& block, u32 s ) -> Site
        {
            const Statement& statement = caller.statements[block.first_statement + s];
            if( statement.kind != Statement_kind::Drop || statement.drop_flag.is_valid() || !statement.value.callee.is_valid() )
            {
                return {};
            }

            for( u32 j = 0; j < functions.size(); ++j )
            {
                if( !finished[j] )
                {
                    continue;
                }

                if( calls( statement, functions[j] ) )
                {
                    if( replaceable_receiver( functions[j] ) &&
                        inline_cost( functions[j], weights ) <= threshold( statement_count, functions[j], weights ) )
                    {
                        return Site { .callee = &functions[j], .receiver = statement.place };
                    }
                    else
                    {
                        return {};
                    }
                }
            }

            return {};
        }
    );
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

    std::size_t drops( bool flagged ) const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s ) { return s.kind == Statement_kind::Drop && s.drop_flag.is_valid() == flagged; }
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

// `(*this).value` becomes `b.value`, so `b` is built where it lies.
TEST_CASE( "inline_calls_inlines_a_constructor", "[opt][inline]" )
{
    const Inlined p( "i64 f() { Box b = Box( 4 ); return b.value; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 0 );
}

// `fill` is over the threshold and takes `this` itself, which no place can stand in for.
TEST_CASE( "inline_calls_keeps_a_constructor_that_passes_this_on", "[opt][inline]" )
{
    const Inlined p( "i32 f() { Reg r = Reg( 4 ); return r.value; }\n"
                     "class Reg { public i32 value; public Reg( i32 v ) { value = v; fill(); }"
                     " void fill() { value = mixed( 1, 2 ) + mixed( 3, 4 ) + mixed( 5, 6 ); } };\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 1 );
}

TEST_CASE( "inline_drops_inlines_an_unconditional_drop", "[opt][inline]" )
{
    const Inlined p( "i64 f() { Box b = Box( 4 ); return b.value; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( false ) == 0 );
}

// Whether `~Box` runs is only known at run time, so the drop stays.
TEST_CASE( "inline_drops_keeps_a_flagged_drop", "[opt][inline]" )
{
    const Inlined p( "i64 f( bool c ) { Box b = Box( 3 ); if( c ) { consume( move b ); } return 0; }\n"
                     "void consume( move Box b ) { }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( true ) == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
