// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/initialised.h"

namespace keel
{

namespace
{
// Sets `local`'s own entry and every leaf in `range` to holding a value, or to holding none.
void fill( Occupancy& state, Local_id local, const Slots& range, bool filled )
{
    state.initialised[local.v]   = filled;
    state.uninitialised[local.v] = !filled;

    for( u32 slot = range.first; slot < range.last; ++slot )
    {
        state.initialised[slot]   = filled;
        state.uninitialised[slot] = !filled;
    }
}

void mark_storage( Occupancy& state, const Slot_map& slots, Local_id local, bool dead )
{
    const Slots all = slots.under( local.v );
    fill( state, local, all, false );
    state.storage_dead[local.v] = dead;

    for( u32 slot = all.first; slot < all.last; ++slot )
    {
        state.storage_dead[slot] = dead;
    }
}

// Every parameter arrives holding a value; a local with a storage marker starts outside it.
Occupancy entry_occupancy( const Function& func, const Slot_map& slots )
{
    Occupancy state {};
    state.initialised.resize( slots.count(), false );
    state.storage_dead.resize( slots.count(), false );
    state.uninitialised.resize( slots.count(), true );

    for( u32 i = 1; i <= func.parameter_count; ++i )
    {
        fill( state, Local_id { i }, slots.under( i ), true );
    }

    for( const Statement& statement : func.statements )
    {
        if( statement.kind == Statement_kind::Storage_live && !statement.place.is_global() )
        {
            mark_storage( state, slots, statement.place.local, true );
        }
    }

    return state;
}

// The local a place writes, or none: a write through a pointer writes the pointee.
Local_id local_of( const Function& func, const Place& place )
{
    if( place.is_global() || has_deref_projection( func, place ) )
    {
        return Local_id {};
    }

    return place.local;
}

} // namespace

void step( const Function& func, const Slot_map& slots, const Statement& statement, Occupancy& state )
{
    switch( statement.kind )
    {
    case Statement_kind::Assign:
    {
        // Moves first, so `_1 = move _1` ends holding a value. Moving one field leaves the rest.
        for_each_operand(
            func,
            statement.value,
            [&]( const Operand& operand )
            {
                if( operand.kind != Operand_kind::Move || !is_field_path( func, operand.place ) )
                {
                    return;
                }

                const Slots moved = slots.of( operand.place );
                if( operand.place.num_projections == 0 )
                {
                    fill( state, operand.place.local, moved, false );
                }
                else if( moved.exact )
                {
                    for( u32 slot = moved.first; slot < moved.last; ++slot )
                    {
                        state.initialised[slot]   = false;
                        state.uninitialised[slot] = true;
                    }
                }
            }
        );

        // `_3 = &init _2`: the constructor called next fills `_2` through `_3`.
        if( statement.value.kind == Rvalue_kind::Address_of && statement.value.address_purpose == Address_purpose::Initialise )
        {
            const Local_id local = local_of( func, statement.value.a.place );
            if( local.is_valid() )
            {
                fill( state, local, slots.of( statement.value.a.place ), true );
            }
        }

        // A field write counts: a partly built value is still one another local must not overwrite.
        const Local_id local = local_of( func, statement.place );
        if( local.is_valid() )
        {
            fill( state, local, slots.of( statement.place ), true );
        }
        break;
    }
    // `drop _1` runs the destructor's body, and `drop _1.field` drops each owning field after it, so
    // a drop ends nothing; a value ends at a move or a storage marker.
    case Statement_kind::Drop:
    {
        break;
    }
    case Statement_kind::Storage_live:
    case Statement_kind::Storage_dead:
    {
        const Local_id local = local_of( func, statement.place );
        if( local.is_valid() )
        {
            mark_storage( state, slots, local, statement.kind == Statement_kind::Storage_dead );
        }
        break;
    }
    }
}

void step( const Terminator& terminator, Occupancy& state )
{
    if( terminator.condition.kind == Operand_kind::Move && !terminator.condition.place.is_global() &&
        terminator.condition.place.num_projections == 0 )
    {
        state.initialised[terminator.condition.place.local.v]   = false;
        state.uninitialised[terminator.condition.place.local.v] = true;
    }
}

bool writes( const Function& func, const Statement& statement, Local_id local )
{
    if( statement.kind != Statement_kind::Assign )
    {
        return false;
    }

    if( local_of( func, statement.place ) == local )
    {
        return true;
    }

    if( statement.value.kind == Rvalue_kind::Address_of && statement.value.address_purpose == Address_purpose::Initialise &&
        local_of( func, statement.value.a.place ) == local )
    {
        return true;
    }

    return false;
}

std::vector<Occupancy> occupancy_on_entry( const Function& func, const Slot_map& slots )
{
    std::vector<Occupancy> entry( func.blocks.size() );
    entry[0] = entry_occupancy( func, slots );
    for( u32 i = 1; i < func.blocks.size(); ++i )
    {
        entry[i] = Occupancy {
            .initialised   = std::vector<bool>( slots.count(), false ),
            .storage_dead  = std::vector<bool>( slots.count(), false ),
            .uninitialised = std::vector<bool>( slots.count(), false )
        };
    }

    bool                  changed = true;
    std::vector<Block_id> targets;
    while( changed )
    {
        changed = false;

        for( u32 i = 0; i < func.blocks.size(); ++i )
        {
            const Block& block = func.blocks[i];
            Occupancy    state = entry[i];

            for( u32 s = 0; s < block.statement_count; ++s )
            {
                step( func, slots, func.statements[block.first_statement + s], state );
            }

            step( block.terminator, state );

            targets.clear();
            successors( block.terminator, targets );
            for( const Block_id target : targets )
            {
                for( u32 l = 0; l < slots.count(); ++l )
                {
                    if( state.initialised[l] && !entry[target.v].initialised[l] )
                    {
                        entry[target.v].initialised[l] = true;
                        changed                        = true;
                    }

                    if( state.storage_dead[l] && !entry[target.v].storage_dead[l] )
                    {
                        entry[target.v].storage_dead[l] = true;
                        changed                         = true;
                    }

                    if( state.uninitialised[l] && !entry[target.v].uninitialised[l] )
                    {
                        entry[target.v].uninitialised[l] = true;
                        changed                          = true;
                    }
                }
            }
        }
    }

    return entry;
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

constexpr std::string_view k_box = "class Box { public i32 v; public Box( i32 n ) { v = n; } ~Box() { } };\n"
                                   "struct Point { i32 x; i32 y; };\n"
                                   "void consume( move Box b ) { }\n"
                                   "i32 main() { return 0; }\n";

// The function under test is always the first in the source.
struct Analysed : Compiled
{
    explicit Analysed( std::string_view source )
        : Compiled( std::string( source ) + std::string( k_box ) )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    Local_id local( std::string_view name ) const
    {
        const Symbol_id symbol = interner.find( name );

        for( u32 i = 0; i < first().locals.size(); ++i )
        {
            if( first().locals[i].name == symbol )
            {
                return Local_id { i };
            }
        }

        return Local_id {};
    }

    // The state just after the first statement `where` accepts, walking blocks in order.
    Occupancy after( const std::function<bool( const Statement& )>& where ) const
    {
        const Function&              func = first();
        const Slot_map               slots( func );
        const std::vector<Occupancy> entry = occupancy_on_entry( func, slots );

        for( u32 b = 0; b < func.blocks.size(); ++b )
        {
            Occupancy state = entry[b];

            for( u32 i = 0; i < func.blocks[b].statement_count; ++i )
            {
                const Statement& statement = func.statements[func.blocks[b].first_statement + i];
                step( func, slots, statement, state );

                if( where( statement ) )
                {
                    return state;
                }
            }
        }

        FAIL( "no statement matched" );
        return {};
    }
};

} // namespace

TEST_CASE( "initialised_starts_with_the_parameters", "[ir][initialised]" )
{
    const Analysed p( "i32 f( i32 a ) { return a; }\n" );
    REQUIRE( p.clean() );

    const Occupancy entry = occupancy_on_entry( p.first(), Slot_map( p.first() ) )[0];

    REQUIRE( entry.initialised[1] );
    REQUIRE_FALSE( entry.initialised[0] );
}

TEST_CASE( "initialised_is_cleared_by_a_move", "[ir][initialised]" )
{
    const Analysed p( "Box f( move Box b ) { return b; }\n" );
    REQUIRE( p.clean() );

    const Occupancy state = p.after( []( const Statement& s )
                                     { return s.kind == Statement_kind::Assign && s.value.a.kind == Operand_kind::Move; } );

    REQUIRE_FALSE( state.initialised[1] );
    REQUIRE( state.initialised[0] );
}

// `_3 = &init _2` fills `_2` through `_3`, so `_2` holds a value from there on.
TEST_CASE( "initialised_is_set_by_a_constructor's_address", "[ir][initialised]" )
{
    const Analysed p( "Box f() { Box b = Box( 1 ); return b; }\n" );
    REQUIRE( p.clean() );

    const Occupancy state =
        p.after( []( const Statement& s )
                 { return s.value.kind == Rvalue_kind::Address_of && s.value.address_purpose == Address_purpose::Initialise; }
        );

    REQUIRE( state.initialised[p.local( "b" ).v] );
}

TEST_CASE( "initialised_is_set_by_a_field_write", "[ir][initialised]" )
{
    const Analysed p( "i32 f() { Point q = Point { 1, 2 }; return q.x; }\n" );
    REQUIRE( p.clean() );

    Local_id        written {};
    const Occupancy state = p.after(
        [&]( const Statement& s )
        {
            written = s.place.local;
            return s.kind == Statement_kind::Assign && s.place.num_projections == 1;
        }
    );

    REQUIRE( state.initialised[written.v] );
}

// `(*_1).x = …` writes what `_1` points at; no local, and certainly not the return slot, gains a value.
TEST_CASE( "initialised_is_not_set_by_a_write_through_a_pointer", "[ir][initialised]" )
{
    const Analysed p( "void f( Point* p ) { p.x = 1; }\n" );
    REQUIRE( p.clean() );

    const Occupancy state = p.after( []( const Statement& s ) { return s.kind == Statement_kind::Assign; } );

    REQUIRE_FALSE( state.initialised[0] );
}

TEST_CASE( "initialised_survives_a_join_after_a_move_on_one_path", "[ir][initialised]" )
{
    const Analysed p( "i32 f( bool c, move Box b ) { if( c ) { consume( move b ); } return 0; }\n" );
    REQUIRE( p.clean() );

    const Function&              func  = p.first();
    const std::vector<Occupancy> entry = occupancy_on_entry( func, Slot_map( func ) );

    // The join is the block that assigns the return slot.
    bool checked = false;
    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        for( u32 i = 0; i < func.blocks[b].statement_count; ++i )
        {
            const Statement& s = func.statements[func.blocks[b].first_statement + i];
            if( s.kind == Statement_kind::Assign && s.place.local == k_return_slot && s.place.num_projections == 0 )
            {
                REQUIRE( entry[b].initialised[2] );
                checked = true;
            }
        }
    }

    REQUIRE( checked );
}

// A destructor's body runs first and the owning fields drop after it, so a drop alone ends nothing.
TEST_CASE( "initialised_survives_a_drop", "[ir][initialised]" )
{
    const Analysed p( "i32 f() { { Box b = Box( 1 ); } return 0; }\n" );
    REQUIRE( p.clean() );

    const Local_id  b = p.local( "b" );
    const Occupancy state =
        p.after( [&]( const Statement& s ) { return s.kind == Statement_kind::Drop && s.place.local == b; } );

    REQUIRE( state.initialised[b.v] );
}

TEST_CASE( "initialised_ends_at_storage_dead", "[ir][initialised]" )
{
    const Analysed p( "i32 f() { { i32 x = 1; } return 0; }\n" );
    REQUIRE( p.clean() );

    const Local_id  x = p.local( "x" );
    const Occupancy state =
        p.after( [&]( const Statement& s ) { return s.kind == Statement_kind::Storage_dead && s.place.local == x; } );

    REQUIRE_FALSE( state.initialised[x.v] );
    REQUIRE( state.storage_dead[x.v] );
}

// A local declared in a loop's body is outside its storage when the header is first reached.
TEST_CASE( "initialised_starts_a_marked_local_storage_dead", "[ir][initialised]" )
{
    const Analysed p( "i32 f() { for( i32 i = 0; i < 3; i++ ) { i32 x = i; } return 0; }\n" );
    REQUIRE( p.clean() );

    const Function&              func  = p.first();
    const std::vector<Occupancy> entry = occupancy_on_entry( func, Slot_map( func ) );
    const Local_id               x     = p.local( "x" );

    REQUIRE( entry[0].storage_dead[x.v] );

    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        if( func.blocks[b].terminator.kind == Terminator_kind::Branch )
        {
            REQUIRE( entry[b].storage_dead[x.v] );
        }
    }
}

TEST_CASE( "uninitialised_starts_everywhere_but_the_parameters", "[ir][initialised]" )
{
    const Analysed p( "i32 f( i32 a ) { return a; }\n" );
    REQUIRE( p.clean() );

    const Occupancy entry = occupancy_on_entry( p.first(), Slot_map( p.first() ) )[0];

    REQUIRE( entry.uninitialised[0] );
    REQUIRE_FALSE( entry.uninitialised[1] );
}

TEST_CASE( "uninitialised_is_set_by_a_move_and_cleared_by_a_write", "[ir][initialised]" )
{
    SECTION( "a move empties the source and fills the return slot" )
    {
        const Analysed p( "Box f( move Box b ) { return b; }\n" );
        REQUIRE( p.clean() );

        const Occupancy state = p.after( []( const Statement& s )
                                         { return s.kind == Statement_kind::Assign && s.value.a.kind == Operand_kind::Move; } );

        REQUIRE( state.uninitialised[1] );
        REQUIRE_FALSE( state.uninitialised[0] );
    }

    SECTION( "a constructor's address fills the local" )
    {
        const Analysed p( "Box f() { Box b = Box( 1 ); return b; }\n" );
        REQUIRE( p.clean() );

        const Occupancy state = p.after(
            []( const Statement& s )
            { return s.value.kind == Rvalue_kind::Address_of && s.value.address_purpose == Address_purpose::Initialise; }
        );

        REQUIRE_FALSE( state.uninitialised[p.local( "b" ).v] );
    }
}

// Moved on one path only: at the join `b` might hold a value and might not, which is where a flag is needed.
TEST_CASE( "uninitialised_and_initialised_both_hold_after_a_move_on_one_path", "[ir][initialised]" )
{
    const Analysed p( "i32 f( bool c, move Box b ) { if( c ) { consume( move b ); } return 0; }\n" );
    REQUIRE( p.clean() );

    const Function&              func  = p.first();
    const std::vector<Occupancy> entry = occupancy_on_entry( func, Slot_map( func ) );

    bool checked = false;
    for( u32 b = 0; b < func.blocks.size(); ++b )
    {
        for( u32 i = 0; i < func.blocks[b].statement_count; ++i )
        {
            const Statement& s = func.statements[func.blocks[b].first_statement + i];
            if( s.kind == Statement_kind::Assign && s.place.local == k_return_slot && s.place.num_projections == 0 )
            {
                REQUIRE( entry[b].initialised[2] );
                REQUIRE( entry[b].uninitialised[2] );
                checked = true;
            }
        }
    }

    REQUIRE( checked );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
