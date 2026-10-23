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

bool whole_local( const Place& place )
{
    return !place.is_global() && place.num_projections == 0;
}

// Two literal ids for one number compare unequal, which only loses a fact.
bool same( const Operand& a, const Operand& b )
{
    if( a.kind != b.kind )
    {
        return false;
    }

    return a.kind == Operand_kind::Constant ? a.constant == b.constant : a.place.local == b.place.local;
}

// `local` changed: its own fact goes, and so does every copy of it.
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

void step( const Function& func, const Local_set& addressed, const Statement& statement, Values& values )
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
        !whole_local( statement.place ) || addressed[statement.place.local.v] )
    {
        return;
    }

    // A chain resolves in one pass: `_3 = copy _2` after `_2 = const 0.5` records `const 0.5`.
    const Local_id into = statement.place.local;
    const Operand& from = statement.value.a;

    if( from.kind == Operand_kind::Constant && from.constant.is_valid() )
    {
        values[into.v] = from;
    }
    else if( from.kind == Operand_kind::Copy && whole_local( from.place ) && from.place.local != into &&
             !addressed[from.place.local.v] )
    {
        values[into.v] = values[from.place.local.v].value_or( from );
    }
}

std::vector<std::optional<Values>> values_on_entry( const Function& func, const Local_set& addressed )
{
    std::vector<std::optional<Values>> values( func.blocks.size(), std::nullopt );
    values[0].emplace( func.locals.size(), std::nullopt );

    // Facts only go away, and a block is reached once, so this ends.
    bool                  changed = true;
    std::vector<Block_id> targets;
    while( changed )
    {
        changed = false;

        for( u32 b = 0; b < func.blocks.size(); ++b )
        {
            if( !values[b] )
            {
                continue;
            }

            const Block& block = func.blocks[b];
            Values       state = *values[b];

            for( u32 s = 0; s < block.statement_count; ++s )
            {
                step( func, addressed, func.statements[block.first_statement + s], state );
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
                for( u32 l = 0; l < func.locals.size(); ++l )
                {
                    if( ( *entry )[l] && ( !state[l] || !same( *( *entry )[l], *state[l] ) ) )
                    {
                        ( *entry )[l] = std::nullopt;
                        changed       = true;
                    }
                }
            }
        }
    }

    return values;
}

// A copy keeps its projections and takes the source's local; a constant replaces only a whole-local
// read.
void rewrite( Operand& operand, const Values& values )
{
    if( operand.kind != Operand_kind::Copy || operand.place.is_global() || !values[operand.place.local.v] )
    {
        return;
    }

    const Operand& known = *values[operand.place.local.v];
    if( known.kind == Operand_kind::Constant && operand.place.num_projections == 0 )
    {
        operand = constant( known.constant, operand.type );
    }
    else if( known.kind == Operand_kind::Copy )
    {
        operand.place.local = known.place.local;
    }
}
} // namespace

// Facts are locals × blocks; a sparse map per block if it shows.
void propagate_values( Function& func, const Type_table& types, Literal_pool& literals )
{
    const Local_set                          addressed = addressed_locals( func );
    const std::vector<std::optional<Values>> entry     = values_on_entry( func, addressed );

    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        if( !entry[b] )
        {
            continue;
        }

        Block& block  = func.blocks[b];
        Values values = *entry[b];

        for( u32 s = 0; s < block.statement_count; ++s )
        {
            Statement& statement = func.statements[block.first_statement + s];

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

            step( func, addressed, statement, values );
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
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Propagated : Compiled
{
    explicit Propagated( std::string_view source )
        : Compiled( std::string( source ) + std::string( k_types ), Stage::Elaborated, Build { .optimise = true } )
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

// `b` (`_2`) is the old `a`, not the `5`; the only read of `a` left is `b = a`.
TEST_CASE( "propagate_values_keeps_a_copy_whose_source_changed", "[opt][propagate]" )
{
    const Propagated p( "i32 f( i32 a ) { i32 b = a; a = 5; return a + b; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.reads( 2 ) == 1 );
    REQUIRE( p.reads( 1 ) == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
