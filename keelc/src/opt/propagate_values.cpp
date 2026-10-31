// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/propagate_values.h"
#include <optional>
#include "ir/live.h"
#include "opt/fold_constants.h"

namespace keel
{

namespace
{
// What each local is known to hold, by Local_id::v: a constant, a copy of a whole local, or nothing known.
// propagate_constants has already read every integer one; a float or an `enum` is left to this.
using Values = std::vector<std::optional<Operand>>;

// A place a fact may name: a whole local or a field of one, never through a pointer.
bool copyable( const Function& func, const Place& place )
{
    return place.local.is_valid() && !place.is_global() && !has_deref_projection( func, place );
}

// Two literal ids for one number compare unequal, which only loses a fact.
bool same( const Operand& a, const Operand& b )
{
    if( a.kind != b.kind )
    {
        return false;
    }

    if( a.kind == Operand_kind::Copy )
    {
        return a.place.local == b.place.local && a.place.num_projections == b.place.num_projections &&
               ( a.place.num_projections == 0 || a.place.first_projection == b.place.first_projection );
    }

    return a.kind == Operand_kind::Constant ? a.constant == b.constant : a.place.local == b.place.local;
}

// `local` changed: its own fact goes, and so does every fact naming it, a field of it included.
void kill( Values& values, Local_id local )
{
    values[local.v] = std::nullopt;
    for( u32 i = 0; i < values.size(); ++i )
    {
        if( values[i].has_value() && values[i]->kind == Operand_kind::Copy && values[i]->place.local == local )
        {
            values[i] = std::nullopt;
        }
    }
}

void step(
    const Function& func, const Type_table& types, const Local_set& addressed, const Statement& statement, Values& values
)
{
    switch( statement.kind )
    {
    case Statement_kind::Assign:
        // A field write changes its local too; a write through a pointer reaches only an addressed one.
        if( !statement.place.is_global() && !has_deref_projection( func, statement.place ) )
        {
            kill( values, statement.place.local );
        }
        // `addressed_locals` leaves out `&init`, but the call it feeds writes the local.
        if( statement.value.kind == Rvalue_kind::Address_of && statement.value.address_purpose == Address_purpose::Initialise &&
            !statement.value.a.place.is_global() )
        {
            kill( values, statement.value.a.place.local );
        }
        break;
    // A copy must not outlive its source's storage.
    case Statement_kind::Drop:
    case Statement_kind::Storage_live:
    case Statement_kind::Storage_dead:
        kill( values, statement.place.local );
        break;
    }

    if( statement.kind != Statement_kind::Assign || statement.value.kind != Rvalue_kind::Use ||
        !statement.place.is_whole_local() || addressed[statement.place.local.v] )
    {
        return;
    }

    // A converting assignment, `i64 w = n`, records nothing: a read of `w` must stay an `i64`.
    const Type_id   to   = func.locals[statement.place.local].type;
    const Type_kind kind = types.get( to ).kind;
    if( statement.value.a.type != to && ( kind == Type_kind::Int || kind == Type_kind::Float ) )
    {
        return;
    }

    // A fact names its direct source, so a later sweep never finds it changed; `rewrite` follows the chain.
    const Local_id into = statement.place.local;
    const Operand& from = statement.value.a;

    if( from.kind == Operand_kind::Constant && from.constant.is_valid() )
    {
        values[into.v] = from;
    }
    else if( from.kind == Operand_kind::Copy && copyable( func, from.place ) && from.place.local != into &&
             !addressed[from.place.local.v] )
    {
        values[into.v] = from;
    }
}

std::vector<std::optional<Values>> values_on_entry( const Function& func, const Type_table& types, const Local_set& addressed )
{
    std::vector<std::optional<Values>> values( func.blocks.size(), std::nullopt );
    values[0].emplace( func.locals.size(), std::nullopt );

    // Facts only go away, and a block is reached once, so this ends.
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
                step( func, types, addressed, statement, state );
            }

            targets.clear();
            successors( block.terminator, targets );
            for( const Block_id target : targets )
            {
                std::optional<Values>& entry = values[target.v];

                if( !entry )
                {
                    entry   = state;
                    changed = true;
                    continue;
                }

                // A fact survives a join only if this path holds it too.
                for( const Local_id l : func.locals.ids() )
                {
                    if( ( *entry )[l.v] && ( !state[l.v] || !same( *( *entry )[l.v], *state[l.v] ) ) )
                    {
                        ( *entry )[l.v] = std::nullopt;
                        changed         = true;
                    }
                }
            }
        }
    }

    return values;
}

// Follows whole-local copies while the operand can take the next fact. A whole-local copy keeps the
// operand's projections and takes its local; a field copy or a constant replaces only a whole-local
// read.
void rewrite( Operand& operand, const Values& values )
{
    if( operand.kind != Operand_kind::Copy || operand.place.is_global() || !values[operand.place.local.v] )
    {
        return;
    }

    const auto takes = [&]( const Operand& fact )
    { return operand.place.num_projections == 0 || ( fact.kind == Operand_kind::Copy && fact.place.num_projections == 0 ); };

    Operand known = *values[operand.place.local.v];
    while( known.kind == Operand_kind::Copy && known.place.num_projections == 0 && values[known.place.local.v] &&
           takes( *values[known.place.local.v] ) )
    {
        known = *values[known.place.local.v];
    }

    if( known.kind == Operand_kind::Constant && operand.place.num_projections == 0 )
    {
        operand = constant( known.constant, operand.type );
    }
    else if( known.kind == Operand_kind::Copy )
    {
        if( known.place.num_projections > 0 && operand.place.num_projections == 0 )
        {
            operand.place = known.place;
        }
        else if( known.place.num_projections == 0 )
        {
            operand.place.local = known.place.local;
        }
    }
}
} // namespace

// Facts are locals × blocks; a sparse map per block if it shows.
void propagate_values( Function& func, const Type_table& types, Literal_pool& literals )
{
    const Local_set                          addressed = addressed_locals( func );
    const std::vector<std::optional<Values>> entry     = values_on_entry( func, types, addressed );

    for( const Block_id b : func.blocks.ids() )
    {
        if( !entry[b.v] )
        {
            continue;
        }

        Block& block  = func.blocks[b];
        Values values = *entry[b.v];

        for( u32 s = 0; s < block.statement_count; ++s )
        {
            Statement& statement = func.statements_of( block )[s];

            // Reads first, so `_1 = copy _1 + const 1` reads the old `_1`. `&_2`'s operand is a
            // Constant by default, so an address never moves.
            if( statement.kind == Statement_kind::Assign )
            {
                rewrite( statement.value.a, values );
                rewrite( statement.value.b, values );

                for( u32 i = 0; i < statement.value.argument_count; ++i )
                {
                    rewrite( func.operands[statement.value.first_argument + i], values );
                }

                // Before the step, so `_3 = copy _45 + const 0` records a copy.
                if( std::optional<Rvalue> folded = identity( statement.value, types, literals ) )
                {
                    statement.value = std::move( *folded );
                }
            }

            step( func, types, addressed, statement, values );
        }

        rewrite( block.terminator.condition, values );
        rewrite( block.terminator.message, values );
        rewrite( block.terminator.at, values );
    }
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

constexpr std::string_view k_types = "void set( out i32 n ) { n = 9; }\n"
                                     "struct Point { i32 x; i32 y; };\n"
                                     "void touch( Point p, out i32 r ) { r = p.y; }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Propagated : Compiled
{
    explicit Propagated( std::string_view source, u32 rounds = 2 )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build { .optimise = true, .rounds = rounds, .recoalescing = false }
          )
    {
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
                for_each_operand( first(), statement.value, note );
            }
        }

        for( const Block& block : first().blocks )
        {
            note( block.terminator.condition );
        }

        return count;
    }
};

} // namespace

// Nothing assigns `x` (`_1`) between `0` and the read, but `set` writes it through `&init`.
TEST_CASE( "propagate_values_keeps_a_local_an_out_argument_rewrites", "[opt][propagate]" )
{
    const Propagated p( "i32 f() { i32 x = 0; set( out x ); return x + 1; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.reads( 1 ) == 1 );
}

// `b` (`_2`) is the old `a`, not the `5`; the only read of `a` left is `b = a`. One round, as the next
// finds `a = 5` gone and forwards `b`.
TEST_CASE( "propagate_values_keeps_a_copy_whose_source_changed", "[opt][propagate]" )
{
    const Propagated p( "i32 f( i32 a ) { i32 b = a; a = 5; return a + b; }\n", 1 );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.reads( 2 ) == 1 );
    REQUIRE( p.reads( 1 ) == 1 );
}

// The split `Box`'s field reaches the add through four copies, inside a loop whose counter the first
// sweep sees as `0`; the add reads the counter.
TEST_CASE( "propagate_values_follows_a_chain_round_a_loop", "[opt][propagate]" )
{
    const Propagated p(
        "i64 f( i64 n ) { i64 t = 0; for( i64 i = 0; i < n; i++ ) { t += take( pass( Box( i ) ) ); } return t; }\n"
        "class Box { public i64 value; public Box( i64 v ) { value = v; } ~Box() { } };\n"
        "i64 take( move Box b ) { return b.value; }\n"
        "Box pass( move Box b ) { return move b; }\n"
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( std::ranges::none_of(
        p.first().statements,
        []( const Statement& s )
        { return s.kind == Statement_kind::Assign && s.value.kind == Rvalue_kind::Use && s.value.a.kind == Operand_kind::Copy; }
    ) );
}

// `p` goes whole to a call, so stays whole; `a` (`_2`) is read as `p.x`.
TEST_CASE( "propagate_values_forwards_a_copy_of_a_field", "[opt][propagate]" )
{
    const Propagated p( "i32 f( Point p ) { i32 a = p.x; i32 r = 0; touch( p, out r ); return a * a + r; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.reads( 2 ) == 0 );
}

TEST_CASE( "propagate_values_keeps_a_field_copy_written_after", "[opt][propagate]" )
{
    const Propagated p( "i32 f( Point p ) { i32 a = p.x; p.x = 5; i32 r = 0; touch( p, out r ); return a * a + r; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.reads( 2 ) == 2 );
}

// `a` (`_3`) is `p.x` on one path and `p.y` on the other, so the join keeps neither.
TEST_CASE( "propagate_values_keeps_two_fields_apart_at_a_join", "[opt][propagate]" )
{
    const Propagated p(
        "i32 f( Point p, bool c ) { i32 a = 0; if( c ) { a = p.x; } else { a = p.y; } i32 r = 0; touch( p, out r ); "
        "return a * a + r; }\n"
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.reads( 3 ) == 2 );
}

// `w` (`_2`) widens `n`, so `w * w` stays an `i64` multiply of `w`.
TEST_CASE( "propagate_values_keeps_a_widening_copy", "[opt][propagate]" )
{
    const Propagated p( "i64 f( i32 n ) { i64 w = n; return w * w; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.reads( 2 ) == 2 );
}

// `y == x` reads `x` twice once propagated, so SCCP folds the `!` and the branch on it.
TEST_CASE( "propagate_values_folds_a_self_comparison_before_its_branch", "[opt][propagate]" )
{
    const Propagated p( "i32 f( i32 x ) { i32 y = x; return !( y == x ) ? 1 : 0; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( std::ranges::none_of(
        p.first().blocks, []( const Block& block ) { return block.terminator.kind == Terminator_kind::Branch; }
    ) );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
