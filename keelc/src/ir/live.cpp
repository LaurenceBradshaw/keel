// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/live.h"

namespace keel
{

namespace
{
// An index is read even in a place being written: `_2[_5] = …` reads `_5`.
void read_indices( const Function& func, const Place& place, Local_set& live )
{
    for( u32 i = 0; i < place.num_projections; ++i )
    {
        const Projection& projection = func.projections[place.first_projection + i];

        if( projection.kind == Projection_kind::Index && projection.index.is_valid() )
        {
            live[projection.index.v] = true;
        }
    }
}

// A read of `_2.x` is a read of `_2`.
void read( const Function& func, const Place& place, Local_set& live )
{
    if( !place.is_global() )
    {
        live[place.local.v] = true;
    }

    read_indices( func, place, live );
}

// Turns live after `statement` into live before it. Kill first, then reads, so `_1 = copy _1 + const 1`
// leaves `_1` live.
void step_back( const Function& func, const Statement& statement, Local_set& live )
{
    switch( statement.kind )
    {
    case Statement_kind::Assign:
        // A field write kills nothing: the other fields still hold what they did.
        if( !statement.place.is_global() && statement.place.num_projections == 0 )
        {
            live[statement.place.local.v] = false;
        }
        read_indices( func, statement.place, live );
        // `(*_3).x = …` reads the pointer `_3`.
        if( has_deref_projection( func, statement.place ) )
        {
            live[statement.place.local.v] = true;
        }
        for_each_operand(
            func,
            statement.value,
            [&]( const Operand& operand )
            {
                if( operand.has_place() )
                {
                    read( func, operand.place, live );
                }
            }
        );
        // `&init _2` reads nothing of `_2`; for_each_operand skips this operand.
        if( statement.value.kind == Rvalue_kind::Address_of )
        {
            if( statement.value.address_purpose != Address_purpose::Initialise )
            {
                read( func, statement.value.a.place, live );
            }
            else
            {
                read_indices( func, statement.value.a.place, live );
            }
        }
        break;
    case Statement_kind::Drop:
        // A drop reads what it destroys, so a class stays live from `drop _1` through `drop _1.inner`.
        read( func, statement.place, live );
        break;
    case Statement_kind::Storage_live:
    case Statement_kind::Storage_dead:
        if( !statement.place.is_global() )
        {
            live[statement.place.local.v] = false;
        }
        break;
    }
}

// Returning reads the return slot.
void step_back( const Function& func, const Terminator& terminator, Local_set& live )
{
    if( terminator.condition.has_place() )
    {
        read( func, terminator.condition.place, live );
    }

    if( terminator.message.has_place() )
    {
        read( func, terminator.message.place, live );
    }

    if( terminator.at.has_place() )
    {
        read( func, terminator.at.place, live );
    }

    if( terminator.kind == Terminator_kind::Return )
    {
        live[k_return_slot.v] = true;
    }
}

} // namespace

Liveness live_locals( const Function& func )
{
    Liveness live;
    live.on_entry.resize( func.blocks.size() );
    live.after.resize( func.statements.size() );

    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        Local_set& block_live = live.on_entry[b];
        block_live.resize( func.locals.size(), false );
    }

    for( u32 s = 0; s < func.statements.size(); ++s )
    {
        Local_set& statement_live = live.after[s];
        statement_live.resize( func.locals.size(), false );
    }

    // Bits only go from false to true across rounds, so this ends; the last round rewrites every `after`.
    std::vector<Block_id> targets;
    bool                  changed = true;
    while( changed )
    {
        changed = false;

        // Any order reaches the same answer; backward needs fewer rounds.
        for( u32 b = static_cast<u32>( func.blocks.size() ); b-- > 0; )
        {
            const Block& block = func.blocks[b];
            // Live at the exit: whatever any successor reads.
            Local_set state( func.locals.size(), false );
            targets.clear();
            successors( block.terminator, targets );

            for( Block_id target : targets )
            {
                for( u32 l = 0; l < func.locals.size(); ++l )
                {
                    if( live.on_entry[target.v][l] )
                    {
                        state[l] = true;
                    }
                }
            }

            step_back( func, block.terminator, state );

            for( u32 i = block.statement_count; i-- > 0; )
            {
                const u32 at   = block.first_statement + i;
                live.after[at] = state;
                step_back( func, func.statements[at], state );
            }

            if( state != live.on_entry[b] )
            {
                live.on_entry[b] = state;
                changed          = true;
            }
        }
    }

    return live;
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include <functional>

#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

constexpr std::string_view k_types = "struct Point { i32 x; i32 y; };\n"
                                     "class Box { public i32 v; public Box( i32 n ) { v = n; } ~Box() { } };\n"
                                     "class Holder { public Box b; public Holder( i32 n ) { b = Box( n ); } ~Holder() { } };\n"
                                     "i32 main() { return 0; }\n";

// The function under test is always the first in the source.
struct Analysed : Compiled
{
    explicit Analysed( std::string_view source )
        : Compiled( std::string( source ) + std::string( k_types ) ),
          live( live_locals( functions.front() ) )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    // The index of the first statement `where` accepts.
    u32 find( const std::function<bool( const Statement& )>& where ) const
    {
        for( u32 i = 0; i < first().statements.size(); ++i )
        {
            if( where( first().statements[i] ) )
            {
                return i;
            }
        }

        FAIL( "no statement matched" );
        return 0;
    }

    // The block whose first statement `where` accepts.
    u32 block( const std::function<bool( const Statement& )>& where ) const
    {
        for( u32 b = 0; b < first().blocks.size(); ++b )
        {
            const Block& block = first().blocks[b];
            if( block.statement_count > 0 && where( first().statements[block.first_statement] ) )
            {
                return b;
            }
        }

        FAIL( "no block matched" );
        return 0;
    }

    Liveness live;
};

std::function<bool( const Statement& )> assigns( u32 local, u32 projections = 0 )
{
    return [=]( const Statement& s )
    { return s.kind == Statement_kind::Assign && s.place.local.v == local && s.place.num_projections == projections; };
}

} // namespace

// `_2 = copy _1 + const 1; _0 = copy _2 * const 2`: `_2` is live between its write and its read.
TEST_CASE( "live_from_a_write_to_the_last_read", "[ir][live]" )
{
    const Analysed p( "i32 f( i32 a ) { i32 x = a + 1; return x * 2; }\n" );
    REQUIRE( p.clean() );

    const u32 write = p.find( assigns( 2 ) );
    const u32 read  = p.find( assigns( 0 ) );

    REQUIRE( p.live.after[write][2] );
    REQUIRE_FALSE( p.live.after[write][1] );
    REQUIRE_FALSE( p.live.after[read][2] );
}

// Before `_2 = …` nothing reads `_2`; before `_3 = copy _3 + const 1` the value is still wanted.
TEST_CASE( "live_is_killed_by_a_whole_write_unless_it_reads_the_local", "[ir][live]" )
{
    const Analysed p( "i32 f( i32 n ) { i32 t = 0; for( i32 i = 0; i < n; i++ ) { t = t + i; } return t; }\n" );
    REQUIRE( p.clean() );

    REQUIRE_FALSE( p.live.on_entry[0][2] );

    const u32 step = p.block( assigns( 3 ) );
    REQUIRE( p.live.on_entry[step][3] );
}

// `_1.x = const 3` leaves `_1.y` as it was, so `_1` is live just before it.
TEST_CASE( "live_survives_a_field_write", "[ir][live]" )
{
    const Analysed p( "i32 f() { Point q = Point { 1, 2 }; q.x = 3; return q.y; }\n" );
    REQUIRE( p.clean() );

    REQUIRE( p.live.after[p.find( assigns( 1, 1 ) ) - 1][1] );
}

// `drop _1` runs the destructor's body; `drop _1.b` still reads `_1` after it.
TEST_CASE( "live_survives_a_drop_until_its_fields_drop", "[ir][live]" )
{
    const Analysed p( "i32 f() { { Holder h = Holder( 1 ); } return 0; }\n" );
    REQUIRE( p.clean() );

    const u32 whole =
        p.find( []( const Statement& s ) { return s.kind == Statement_kind::Drop && s.place.num_projections == 0; } );
    const u32 field =
        p.find( []( const Statement& s ) { return s.kind == Statement_kind::Drop && s.place.num_projections == 1; } );

    REQUIRE( p.live.after[whole][1] );
    REQUIRE_FALSE( p.live.after[field][1] );
}

TEST_CASE( "live_holds_the_return_slot_at_return", "[ir][live]" )
{
    const Analysed p( "i32 f( i32 a ) { i32 x = a + 1; return x * 2; }\n" );
    REQUIRE( p.clean() );

    const Function& func = p.first();
    const Block&    last = func.blocks[0];
    REQUIRE( p.live.after[last.first_statement + last.statement_count - 1][0] );
}

// `n` and `t` are read at or after the header, so the step at the end of the body keeps them.
TEST_CASE( "live_reaches_round_a_loop", "[ir][live]" )
{
    const Analysed p( "i32 f( i32 n ) { i32 t = 0; for( i32 i = 0; i < n; i++ ) { t = t + i; } return t; }\n" );
    REQUIRE( p.clean() );

    const Block& step = p.first().blocks[p.block( assigns( 3 ) )];
    const u32    end  = step.first_statement + step.statement_count - 1;

    REQUIRE( p.live.after[end][1] );
    REQUIRE( p.live.after[end][2] );
}

// `_3[_4] = const 1` reads `_4` though it writes `_3`; `copy _2[_7]` reads `_7` in another block.
TEST_CASE( "live_holds_an_index", "[ir][live]" )
{
    const Analysed p( "i32 f( u64 k ) { i32[3] a = [1, 2, 3]; return a[k]; }\n" );
    REQUIRE( p.clean() );

    REQUIRE( p.live.after[p.find( assigns( 4 ) )][4] );
    REQUIRE( p.live.after[p.find( assigns( 8 ) )][7] );
}

TEST_CASE( "writes_sees_a_field_and_a_constructor's_address_not_a_pointee", "[ir][live]" )
{
    const Analysed field( "i32 f() { Point q = Point { 1, 2 }; return q.y; }\n" );
    REQUIRE( field.clean() );
    REQUIRE( writes( field.first(), field.first().statements[field.find( assigns( 2, 1 ) )], Local_id { 2 } ) );

    const Analysed built( "i32 f() { { Holder h = Holder( 1 ); } return 0; }\n" );
    REQUIRE( built.clean() );
    const u32 init = built.find( []( const Statement& s ) { return s.value.kind == Rvalue_kind::Address_of; } );
    REQUIRE( writes( built.first(), built.first().statements[init], Local_id { 1 } ) );

    const Analysed through( "void f( Point* p ) { p.x = 1; }\n" );
    REQUIRE( through.clean() );
    REQUIRE_FALSE( writes( through.first(), through.first().statements[0], Local_id { 1 } ) );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
