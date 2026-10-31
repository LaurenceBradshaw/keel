// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/propagate_constants.h"
#include <optional>
#include "ir/live.h"
#include "ir/simplify.h"
#include "opt/fold_constants.h"

namespace keel
{

namespace
{

// What a local holds: no write reached yet, one value as its bits at its type's width, or more than one.
struct Value
{
    enum class State : u8
    {
        Undefined,
        Constant,
        Varying
    };
    State state = State::Undefined;
    u64   bits  = 0;
};

using Values = std::vector<Value>;

Value join( Value a, Value b )
{
    if( a.state == Value::State::Undefined )
    {
        return b;
    }

    if( b.state == Value::State::Undefined )
    {
        return a;
    }

    if( a.state == Value::State::Constant && b.state == Value::State::Constant && a.bits == b.bits )
    {
        return a;
    }

    return Value { .state = Value::State::Varying };
}

bool is_integer( const Type_table& types, Type_id type )
{
    const Type_kind kind = types.get( type ).kind;

    if( kind == Type_kind::Int || kind == Type_kind::Bool )
    {
        return true;
    }

    return false;
}

// Integers and `bool` no pointer reaches; every other read is Varying.
Local_set tracked_locals( const Function& func, const Type_table& types )
{
    const Local_set addressed = addressed_locals( func );
    Local_set       tracked( func.locals.size(), false );

    for( const Local_id l : func.locals.ids() )
    {
        if( !addressed[l.v] && is_integer( types, func.locals[l].type ) )
        {
            tracked[l.v] = true;
        }
    }

    return tracked;
}

struct Analysis
{
    const Function&     func;
    const Type_table&   types;
    const Literal_pool& literals;
    Local_set           tracked;
};

bool whole_tracked( const Analysis& analysis, const Place& place )
{
    return place.is_whole_local() && analysis.tracked[place.local.v];
}

Value read( const Analysis& analysis, const Operand& operand, const Values& values )
{
    if( operand.kind == Operand_kind::Constant && operand.constant.is_valid() && is_integer( analysis.types, operand.type ) )
    {
        return Value { .state = Value::State::Constant, .bits = analysis.literals.integer( operand.constant ) };
    }

    if( operand.kind == Operand_kind::Copy && whole_tracked( analysis, operand.place ) )
    {
        return values[operand.place.local.v];
    }

    // A `move` keeps its read, as in propagate_values.
    return Value { .state = Value::State::Varying };
}

Value evaluated( const Analysis& analysis, const Rvalue& value, const Values& values )
{
    if( value.kind != Rvalue_kind::Use && value.kind != Rvalue_kind::Binary && value.kind != Rvalue_kind::Overflows &&
        value.kind != Rvalue_kind::Unary && value.kind != Rvalue_kind::Cast )
    {
        return Value { .state = Value::State::Varying };
    }

    // An operand a kind does not use is the default Constant, with no id, so it reads Varying.
    const Value a = read( analysis, value.a, values );
    const Value b = read( analysis, value.b, values );

    if( value.kind == Rvalue_kind::Use )
    {
        return a;
    }

    // Not known yet: a later sweep brings the write.
    if( a.state == Value::State::Undefined || b.state == Value::State::Undefined )
    {
        return Value { .state = Value::State::Undefined };
    }

    const auto known = [&]( Value v ) -> std::optional<u64>
    {
        if( v.state == Value::State::Constant )
        {
            return v.bits;
        }

        return std::nullopt;
    };

    const std::optional<u64> result = evaluate( value, known( a ), known( b ), analysis.types );

    if( !result )
    {
        return Value { .state = Value::State::Varying };
    }

    return Value { .state = Value::State::Constant, .bits = *result };
}

void step( const Analysis& analysis, const Statement& statement, Values& values )
{
    if( statement.kind != Statement_kind::Assign )
    {
        return;
    }

    const Rvalue& rvalue = analysis.func.rvalue( statement );

    // `addressed_locals` leaves out `&init`, but the call it feeds writes the local.
    if( rvalue.kind == Rvalue_kind::Address_of && rvalue.address_purpose == Address_purpose::Initialise &&
        !rvalue.a.place.is_global() && analysis.tracked[rvalue.a.place.local.v] )
    {
        values[rvalue.a.place.local.v] = Value { .state = Value::State::Varying };
    }

    if( !whole_tracked( analysis, statement.place ) )
    {
        return;
    }

    Value value = evaluated( analysis, rvalue, values );

    // An assignment converts: `i64 w = n` reads an `i32`.
    if( value.state == Value::State::Constant )
    {
        const Type_id            to        = analysis.func.locals[statement.place.local].type;
        const std::optional<u64> converted = convert( value.bits, rvalue.type, to, analysis.types );
        value                              = converted ? Value { .state = Value::State::Constant, .bits = *converted }
                                                       : Value { .state = Value::State::Varying };
    }

    values[statement.place.local.v] = value;
}

std::vector<std::optional<Values>> values_on_entry( const Analysis& analysis )
{
    const Function& func = analysis.func;

    std::vector<std::optional<Values>> values( func.blocks.size(), std::nullopt );
    values[0].emplace( func.locals.size(), Value {} );
    for( u32 l = 1; l <= func.parameter_count; ++l )
    {
        ( *values[0] )[l] = Value { .state = Value::State::Varying };
    }

    // Values only rise and a block is reached once, so this ends.
    bool                  changed = true;
    std::vector<Block_id> targets;
    while( changed )
    {
        changed = false;

        for( const Block_id b : func.blocks.ids() )
        {
            if( !values[b.v] )
            {
                continue;
            }

            const Block& block = func.blocks[b];
            Values       state = *values[b.v];

            for( const Statement& statement : func.statements_of( block ) )
            {
                step( analysis, statement, state );
            }

            // A branch on a constant takes one edge, and on a value not yet known, none.
            targets.clear();
            const Terminator& terminator = block.terminator;
            if( terminator.kind == Terminator_kind::Goto )
            {
                targets.push_back( terminator.targets[0] );
            }
            else if( terminator.kind == Terminator_kind::Branch )
            {
                const Value condition = read( analysis, terminator.condition, state );
                if( condition.state == Value::State::Constant )
                {
                    targets.push_back( terminator.targets[condition.bits != 0 ? 0 : 1] );
                }
                else if( condition.state == Value::State::Varying )
                {
                    targets.push_back( terminator.targets[0] );
                    targets.push_back( terminator.targets[1] );
                }
            }

            for( const Block_id target : targets )
            {
                std::optional<Values>& entry = values[target.v];

                if( !entry )
                {
                    entry   = state;
                    changed = true;
                    continue;
                }

                for( const Local_id l : func.locals.ids() )
                {
                    const Value joined = join( ( *entry )[l.v], state[l.v] );
                    if( joined.state != ( *entry )[l.v].state || joined.bits != ( *entry )[l.v].bits )
                    {
                        ( *entry )[l.v] = joined;
                        changed         = true;
                    }
                }
            }
        }
    }

    return values;
}

// A negative has no operand form, so its read stays.
void rewrite( const Analysis& analysis, Operand& operand, const Values& values, Literal_pool& literals )
{
    if( operand.kind != Operand_kind::Copy || !whole_tracked( analysis, operand.place ) )
    {
        return;
    }

    const Value v = values[operand.place.local.v];

    if( v.state == Value::State::Constant )
    {
        const Rvalue result = value_of( v.bits, operand.type, analysis.types, literals );

        if( result.kind == Rvalue_kind::Use && result.a.kind == Operand_kind::Constant )
        {
            operand = result.a;
        }
    }
}

// Already folded; `add_integer` appends, so folding it again only grows the pool.
bool is_constant( const Rvalue& value )
{
    if( value.a.kind == Operand_kind::Constant &&
        ( value.kind == Rvalue_kind::Use || ( value.kind == Rvalue_kind::Unary && value.op == Token_kind::Minus ) ) )
    {
        return true;
    }

    return false;
}
} // namespace

void propagate_constants( Function& func, const Type_table& types, Literal_pool& literals )
{
    const Analysis analysis { func, types, literals, tracked_locals( func, types ) };
    const auto     entry = values_on_entry( analysis );

    for( const Block_id b : func.blocks.ids() )
    {
        if( !entry[b.v] )
        {
            continue;
        }

        Values values = *entry[b.v];
        Block& block  = func.blocks[b];

        for( u32 s = 0; s < block.statement_count; ++s )
        {
            Statement& statement = func.statements_of( block )[s];
            if( statement.kind != Statement_kind::Assign )
            {
                continue;
            }

            // Before the step, so `_1 = copy _1 + const 1` reads the old `_1`.
            const Value result = evaluated( analysis, func.rvalue( statement ), values );
            // Wherever it is written: a global's or a field's sum folds as a local's does.
            if( result.state == Value::State::Constant && !is_constant( func.rvalue( statement ) ) )
            {
                func.rvalue( statement ) = value_of( result.bits, func.rvalue( statement ).type, types, literals );
            }
            else
            {
                rewrite( analysis, func.rvalue( statement ).a, values, literals );
                rewrite( analysis, func.rvalue( statement ).b, values, literals );

                for( Operand& argument : func.rvalue( statement ).arguments )
                {
                    rewrite( analysis, argument, values, literals );
                }
            }

            step( analysis, statement, values );
        }

        rewrite( analysis, block.terminator.condition, values, literals );
    }

    // A branch that now reads a constant folds, and the arm it never takes goes.
    simplify( func, literals );
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

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Propagated : Compiled
{
    explicit Propagated( std::string_view source )
        : Compiled( std::string( source ) + "i32 main() { return 0; }\n", Stage::Elaborated, Build { .optimise = true } )
    {
        INFO( rendered() );
        REQUIRE( clean() );
        REQUIRE( verify( first() ).empty() );
    }

    const Function& first() const
    {
        return functions.front();
    }

    // How many operands read `local` by copy, statements and terminators both.
    std::size_t reads( u32 local ) const
    {
        std::size_t count = 0;
        const auto  note  = [&]( const Operand& operand )
        {
            if( operand.kind == Operand_kind::Copy && !operand.place.is_global() && operand.place.local.v == local )
            {
                ++count;
            }
        };

        for( const Statement& statement : first().statements )
        {
            if( statement.kind == Statement_kind::Assign )
            {
                for_each_operand( first().rvalue( statement ), note );
            }
        }

        for( const Block& block : first().blocks )
        {
            note( block.terminator.condition );
        }

        return count;
    }

    std::size_t branches() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().blocks, []( const Block& block ) { return block.terminator.kind == Terminator_kind::Branch; }
        ) );
    }

    // Whether some statement assigns the constant `magnitude`, negated if `negative`.
    bool assigns( u64 magnitude, bool negative = false ) const
    {
        return std::ranges::any_of(
            first().statements,
            [&]( const Statement& s )
            {
                const Rvalue& value = first().rvalue( s );

                const bool shape = negative ? value.kind == Rvalue_kind::Unary && value.op == Token_kind::Minus
                                            : value.kind == Rvalue_kind::Use;
                return s.kind == Statement_kind::Assign && shape && value.a.kind == Operand_kind::Constant &&
                       literals.integer( value.a.constant ) == magnitude;
            }
        );
    }
};

} // namespace

// The `||`'s other arm never runs, so its flag is `true` at the join and `f` returns `1` outright.
TEST_CASE( "propagate_constants_takes_only_the_arm_that_runs", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 n ) { if( big( 20 ) || n > 0 ) { return 1; } return 0; }\n"
                        "bool big( i32 x ) { return x > 10; }\n" );
    REQUIRE( p.branches() == 0 );
    REQUIRE( p.first().blocks.size() == 1 );
}

// `k` (`_2`) is `1` round the loop: the only arm that changes it tests `k != 1`, which never holds.
TEST_CASE( "propagate_constants_keeps_a_constant_a_dead_arm_would_change", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 n ) { i32 k = 1; i32 t = 0; for( i32 i = 0; i < n; i++ ) { if( k != 1 ) { k = 2; } "
                        "t = t + k; } return t; }\n" );
    REQUIRE( p.branches() == 1 );
    REQUIRE( p.reads( 2 ) == 0 );
}

// `i` (`_3`) is `0` from the entry, but `1` round the back edge, so the loop keeps reading it.
TEST_CASE( "propagate_constants_keeps_a_loop_counter", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 n ) { i32 t = 0; for( i32 i = 0; i < n; i++ ) { t = t + i; } return t; }\n" );
    REQUIRE( p.branches() == 1 );
    REQUIRE( p.reads( 3 ) == 3 );
}

// `clamp`'s argument is `-4`, so its comparison folds and only `t - 4` is left.
TEST_CASE( "propagate_constants_reads_a_negative_through_a_branch", "[opt][constants]" )
{
    const Propagated p( "i64 f( i64 t ) { i64 a = 3; i64 b = a + 4; return t - clamp( a - b ); }\n"
                        "i64 clamp( i64 x ) { return x < 0 ? 0 - x : x; }\n" );
    REQUIRE( p.branches() == 0 );
    REQUIRE( p.first().blocks.size() == 1 );
}

// `w = n` widens: `w` is `-4` as an `i64`, so `w + 1` is `-3`, not `4294967293`.
TEST_CASE( "propagate_constants_sign_extends_an_assignment", "[opt][constants]" )
{
    const Propagated p( "i64 f() { i32 a = 3; i32 n = a - 7; i64 w = n; return w + 1; }\n" );
    REQUIRE( p.assigns( 3, true ) );
}

// `step` (`_2`) is `2` both from the entry and round the back edge.
TEST_CASE( "propagate_constants_reads_a_constant_round_a_loop", "[opt][constants]" )
{
    const Propagated p( "i32 f( i32 n ) { i32 step = 2; i32 t = 0; for( i32 i = 0; i < n; i++ ) { t = t + step; } return t; }\n"
    );
    REQUIRE( p.reads( 2 ) == 0 );
}

// `k` (`_2`) is `1` on one path and `2` on the other, and both run.
TEST_CASE( "propagate_constants_keeps_a_local_the_paths_disagree_on", "[opt][constants]" )
{
    const Propagated p( "i32 f( bool c ) { i32 k = 1; if( c ) { k = 2; } return k + 1; }\n" );
    REQUIRE( p.reads( 2 ) == 1 );
}

// One pass: `c - 1` is `13` without a round per fold.
TEST_CASE( "propagate_constants_folds_a_chain", "[opt][constants]" )
{
    const Propagated p( "i32 f() { i32 a = 3; i32 b = a + 4; i32 c = b * 2; return c - 1; }\n" );
    REQUIRE( p.assigns( 13 ) );

    for( const Statement& s : p.first().statements )
    {
        REQUIRE( ( s.kind != Statement_kind::Assign || p.first().rvalue( s ).kind != Rvalue_kind::Binary ) );
    }
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
