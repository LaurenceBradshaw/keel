// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/coalesce_moves.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <optional>
#include "ir/initialised.h"
#include "ir/live.h"
#include "ir/simplify.h"

namespace keel
{
namespace
{

struct Candidate
{
    u32      statement;
    Local_id kept;
    Local_id gone;
    Place    into;
};

bool is_parameter( const Function& func, Local_id local )
{
    return local.v >= 1 && local.v <= func.parameter_count;
}

bool is_fixed( const Function& func, Local_id local )
{
    return local == k_return_slot || is_parameter( func, local );
}

bool names( const Place& place, Local_id local )
{
    return !place.is_global() && place.local == local;
}

bool field_place( const Function& func, const Place& place )
{
    if( place.is_global() || place.num_projections == 0 )
    {
        return false;
    }

    bool only_field_or_member = true;
    for( const Projection& projection : func.projections_of( place ) )
    {
        if( projection.kind != Projection_kind::Field && projection.kind != Projection_kind::Member )
        {
            only_field_or_member = false;
            break;
        }
    }

    return only_field_or_member;
}

bool overlaps( const Function& func, const Place& a, const Place& b )
{
    if( a.is_global() || b.is_global() )
    {
        return false;
    }

    if( a.local != b.local )
    {
        return false;
    }

    const u32 shorter = std::min( a.num_projections, b.num_projections );
    for( u32 i = 0; i < shorter; ++i )
    {
        const Projection& a_projection = func.projections_of( a )[i];
        const Projection& b_projection = func.projections_of( b )[i];

        if( a_projection.kind == Projection_kind::Field && b_projection.kind == Projection_kind::Field &&
            a_projection.field != b_projection.field )
        {
            return false;
        }

        if( a_projection.kind == Projection_kind::Member && b_projection.kind == Projection_kind::Member &&
            a_projection.member != b_projection.member )
        {
            return true;
        }

        if( a_projection.kind == b_projection.kind )
        {
            continue;
        }

        if( ( a_projection.kind == Projection_kind::Tag && b_projection.kind == Projection_kind::Field ) ||
            ( a_projection.kind == Projection_kind::Field && b_projection.kind == Projection_kind::Tag ) )
        {
            return false;
        }

        return true;
    }

    return true;
}

bool copies_itself( const Function& func, const Statement& statement )
{
    const Rvalue& value = func.rvalue( statement );

    if( statement.kind != Statement_kind::Assign || value.kind != Rvalue_kind::Use ||
        ( value.a.kind != Operand_kind::Copy && value.a.kind != Operand_kind::Move ) || statement.place.is_global() ||
        value.a.place.is_global() || statement.place.local != value.a.place.local )
    {
        return false;
    }

    if( statement.place.num_projections != value.a.place.num_projections )
    {
        return false;
    }

    for( u32 i = 0; i < statement.place.num_projections; ++i )
    {
        const Projection& place_projection = func.projections_of( statement.place )[i];
        const Projection& value_projection = func.projections_of( value.a.place )[i];

        if( place_projection.kind != value_projection.kind )
        {
            return false;
        }

        if( place_projection.member != value_projection.member || place_projection.field != value_projection.field ||
            place_projection.index != value_projection.index )
        {
            return false;
        }
    }

    return true;
}

bool mentions( const Function& func, const Statement& statement, const Place& into )
{
    const Rvalue& value = func.rvalue( statement );

    if( overlaps( func, statement.place, into ) )
    {
        return true;
    }

    if( value.kind == Rvalue_kind::Address_of && overlaps( func, value.a.place, into ) )
    {
        return true;
    }

    bool found = false;
    for_each_operand(
        value,
        [&]( const Operand& operand )
        {
            if( operand.has_place() && overlaps( func, operand.place, into ) )
            {
                found = true;
            }
        }
    );

    return found;
}

bool mentions( const Function& func, const Terminator& terminator, const Place& into )
{
    if( terminator.condition.has_place() && overlaps( func, terminator.condition.place, into ) )
    {
        return true;
    }

    if( terminator.message.has_place() && overlaps( func, terminator.message.place, into ) )
    {
        return true;
    }

    if( terminator.at.has_place() && overlaps( func, terminator.at.place, into ) )
    {
        return true;
    }

    return false;
}

bool moved_whole( const Function& func, Local_id local, u32 except )
{
    for( u32 i = 0; i < func.statements.size(); ++i )
    {
        if( i == except )
        {
            continue;
        }

        const Statement& statement = func.statements[i];

        bool found = false;
        for_each_operand(
            func.rvalue( statement ),
            [&]( const Operand& operand )
            {
                if( operand.kind == Operand_kind::Move && operand.place.is_whole_local() && names( operand.place, local ) )
                {
                    found = true;
                }
            }
        );

        if( found )
        {
            return true;
        }
    }

    for( const Block_id b : func.blocks.ids() )
    {
        const Terminator& terminator = func.blocks[b].terminator;
        if( terminator.condition.kind == Operand_kind::Move && terminator.condition.place.is_whole_local() &&
            names( terminator.condition.place, local ) )
        {
            return true;
        }

        if( terminator.message.kind == Operand_kind::Move && terminator.message.place.is_whole_local() &&
            names( terminator.message.place, local ) )
        {
            return true;
        }

        if( terminator.at.kind == Operand_kind::Move && terminator.at.place.is_whole_local() &&
            names( terminator.at.place, local ) )
        {
            return true;
        }
    }

    return false;
}

bool indexes( const Function& func, Local_id local )
{
    for( const Projection& projection : func.projections )
    {
        if( projection.kind == Projection_kind::Index && projection.index == local )
        {
            return true;
        }
    }

    return false;
}

// Holds a value something may still see. `written` keeps a dead store: it still lands in the storage.
bool occupied( const Occupancy& state, const Local_set& alive, const Local_set& addressed, Local_id local, bool written )
{
    return state.initialised[local.v] && ( alive[local.v] || addressed[local.v] || written );
}

std::optional<Candidate> candidate_at( const Function& func, u32 index )
{
    const Statement& statement   = func.statements[index];
    const Rvalue&    value       = func.rvalue( statement );
    const Place&     source      = value.a.place;
    const Place&     destination = statement.place;

    if( statement.kind != Statement_kind::Assign || value.kind != Rvalue_kind::Use ||
        !( value.a.kind == Operand_kind::Move || value.a.kind == Operand_kind::Copy ) || destination.is_global() ||
        source.is_global() || destination.local == source.local )
    {
        return {};
    }

    if( source.is_whole_local() && destination.is_whole_local() )
    {
        if( func.locals[destination.local].type != func.locals[source.local].type )
        {
            return {};
        }

        Local_id kept;
        Local_id gone;
        if( is_fixed( func, source.local ) && !is_fixed( func, destination.local ) )
        {
            kept = source.local;
            gone = destination.local;
        }
        else if( !is_fixed( func, source.local ) )
        {
            kept = destination.local;
            gone = source.local;
        }
        else
        {
            return {};
        }

        if( std::ranges::find( func.one_path_temporaries, gone ) != func.one_path_temporaries.end() &&
            is_parameter( func, kept ) )
        {
            return {};
        }

        return Candidate { index, kept, gone, Place { .local = kept } };
    }

    // `_0.a = move _3` builds `_3` in `_0.a`; `_4 = copy _1.b` reads `_1.b` where it lies.
    Candidate field { index, {}, {}, {} };
    if( field_place( func, destination ) && source.is_whole_local() )
    {
        field = Candidate { index, destination.local, source.local, destination };
    }
    else if( destination.is_whole_local() && field_place( func, source ) )
    {
        field = Candidate { index, source.local, destination.local, source };
    }
    else
    {
        return {};
    }

    // A drop flag belongs to a whole local, so neither side may be moved whole elsewhere.
    const auto one_path = [&]( Local_id local )
    { return std::ranges::find( func.one_path_temporaries, local ) != func.one_path_temporaries.end(); };

    if( is_fixed( func, field.gone ) || one_path( field.gone ) || one_path( field.kept ) || indexes( func, field.gone ) ||
        moved_whole( func, field.gone, index ) || moved_whole( func, field.kept, index ) )
    {
        return {};
    }

    return field;
}

bool interferes(
    const Function&               func,
    Local_id                      kept,
    Local_id                      gone,
    const Slot_map&               slots,
    const std::vector<Occupancy>& entry,
    const Liveness&               live,
    const Local_set&              addressed
)
{
    // A dead shell left in `kept`'s storage is fine; a value still wanted is not.
    const auto clash = [&]( const Occupancy& s, const Local_set& alive, bool gone_written, bool kept_written )
    {
        return occupied( s, alive, addressed, gone, gone_written ) &&
               ( occupied( s, alive, addressed, kept, kept_written ) || s.storage_dead[kept.v] );
    };

    for( const Block_id b : func.blocks.ids() )
    {
        const Block& block = func.blocks[b];
        Occupancy    state = entry[b.v];

        if( clash( state, live.on_entry[b.v], false, false ) )
        {
            return true;
        }

        for( u32 i = 0; i < block.statement_count; ++i )
        {
            const u32        at        = func.statements_of( block ).position( i );
            const Statement& statement = func.statements[at];

            // A drop the merge keeps loses the move that guarded it, so on another path it would destroy
            // the other's value: a `switch( move … )` arm that falls through to the scrutinee's drop.
            if( statement.kind == Statement_kind::Drop &&
                ( names( statement.place, gone ) || names( statement.place, kept ) ) && state.initialised[gone.v] &&
                state.initialised[kept.v] )
            {
                return true;
            }

            step( func, slots, statement, state );

            if( clash( state, live.after[at], writes( func, statement, gone ), writes( func, statement, kept ) ) )
            {
                return true;
            }
        }
    }

    return false;
}

// Whether some path after `func.statements[from]` reaches a statement `hit` accepts without passing a
// storage marker of `stop`.
bool reaches( const Function& func, u32 from, Local_id stop, const std::function<bool( u32 )>& hit )
{
    enum class Scan
    {
        Hit,
        Stopped,
        Through
    };

    const auto scan = [&]( const Block& block, u32 start )
    {
        const Run<const Statement> statements = func.statements_of( block );
        for( u32 at = start; at < statements.position( statements.size() ); ++at )
        {
            // A loop can come back round to `from`.
            if( at == from )
            {
                continue;
            }

            if( hit( at ) )
            {
                return Scan::Hit;
            }

            const Statement& statement = func.statements[at];
            if( ( statement.kind == Statement_kind::Storage_live || statement.kind == Statement_kind::Storage_dead ) &&
                names( statement.place, stop ) )
            {
                return Scan::Stopped;
            }
        }

        return Scan::Through;
    };

    const auto holds = [&]( Block_id b )
    {
        const Run<const Statement> statements = func.statements_of( b );
        return from >= statements.position( 0 ) && from < statements.position( statements.size() );
    };

    Block_id home = k_entry_block;
    while( !holds( home ) )
    {
        ++home.v;
    }

    const Scan rest = scan( func.blocks[home], from + 1 );
    if( rest != Scan::Through )
    {
        return rest == Scan::Hit;
    }

    std::vector<bool>     seen( func.blocks.size(), false );
    std::vector<Block_id> work;
    successors( func.blocks[home].terminator, work );

    while( !work.empty() )
    {
        const Block_id b = work.back();
        work.pop_back();

        if( seen[b.v] )
        {
            continue;
        }

        seen[b.v]            = true;
        const Block& block   = func.blocks[b];
        const Scan   scanned = scan( block, block.first_statement );
        if( scanned == Scan::Hit )
        {
            return true;
        }

        if( scanned == Scan::Through )
        {
            successors( block.terminator, work );
        }
    }

    return false;
}

// Whether building `c.gone` in `c.into` could change what anything reads.
bool field_interferes(
    const Function&               func,
    const Candidate&              c,
    const Slot_map&               slots,
    const std::vector<Occupancy>& entry,
    const Liveness&               live,
    const Local_set&              addressed
)
{
    // Which statements run while `gone` holds a value.
    std::vector<bool> in_life( func.statements.size(), false );

    for( const Block_id b : func.blocks.ids() )
    {
        const Block& block = func.blocks[b];
        Occupancy    state = entry[b.v];
        bool         was   = occupied( state, live.on_entry[b.v], addressed, c.gone, false );

        for( u32 i = 0; i < block.statement_count; ++i )
        {
            const u32        at        = func.statements_of( block ).position( i );
            const Statement& statement = func.statements[at];
            step( func, slots, statement, state );

            const bool now = occupied( state, live.after[at], addressed, c.gone, writes( func, statement, c.gone ) );
            // `was` catches the statement that reads `gone` for the last time.
            in_life[at] = was || now;

            // Something else touches the field while `gone` is in it.
            if( at != c.statement && in_life[at] && mentions( func, statement, c.into ) )
            {
                return true;
            }

            if( now && state.storage_dead[c.kept.v] )
            {
                return true;
            }

            was = now;
        }

        if( was && mentions( func, block.terminator, c.into ) )
        {
            return true;
        }
    }

    // A pointer to `kept` reaches the field without naming it, so a borrow that can reach `gone`'s life clashes.
    const auto alive = [&]( u32 i ) { return static_cast<bool>( in_life[i] ); };
    for( u32 at = 0; at < func.statements.size(); ++at )
    {
        const Statement& statement = func.statements[at];
        const Rvalue&    value     = func.rvalue( statement );
        if( statement.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Address_of &&
            names( value.a.place, c.kept ) && ( in_life[at] || reaches( func, at, c.kept, alive ) ) )
        {
            return true;
        }
    }

    // After the merge, a later write to `gone` would change the field too; a borrow may write.
    return reaches(
        func,
        c.statement,
        c.gone,
        [&]( u32 i )
        {
            const Statement& statement = func.statements[i];
            const Rvalue&    value     = func.rvalue( statement );
            return writes( func, statement, c.gone ) ||
                   ( statement.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Address_of &&
                     names( value.a.place, c.gone ) );
        }
    );
}

// `place` with `gone` replaced by `into`: `_3.x` into `_0.a` is `_0.a.x`.
Place moved_to( Function& func, const Place& place, const Place& into )
{
    Place moved = place;
    moved.local = into.local;

    if( into.num_projections == 0 )
    {
        return moved;
    }

    moved.first_projection = narrow_cast<u32>( func.projections.size() );
    moved.num_projections  = into.num_projections + place.num_projections;

    // By value: `push_back` may reallocate under a reference. The old range is left unused.
    for( Projection projection : func.projections_of( into ) )
    {
        func.projections.push_back( projection );
    }

    for( Projection projection : func.projections_of( place ) )
    {
        func.projections.push_back( projection );
    }

    return moved;
}

void rename( Function& func, Local_id gone, const Place& into )
{
    for_each_place(
        func,
        [&]( Place& place, const Statement* )
        {
            if( names( place, gone ) )
            {
                place = moved_to( func, place, into );
            }
        }
    );

    for( Projection& projection : func.projections )
    {
        if( projection.kind == Projection_kind::Index && projection.index == gone )
        {
            // `candidate_at` refuses a field merge of an index.
            assert( into.num_projections == 0 );
            projection.index = into.local;
        }
    }
}

// Marks the move, `gone`'s storage markers and each drop of an uninitialised `gone` or `kept`; answers
// whether a drop of `gone` stays.
bool mark_deletions(
    const Function&               func,
    const Candidate&              chosen,
    const Slot_map&               slots,
    const std::vector<Occupancy>& entry,
    std::vector<bool>&            deleted
)
{
    bool gone_drop_kept = false;
    for( const Block_id b : func.blocks.ids() )
    {
        const Block& block = func.blocks[b];
        Occupancy    state = entry[b.v];

        for( u32 i = 0; i < block.statement_count; ++i )
        {
            const u32        at        = func.statements_of( block ).position( i );
            const Statement& statement = func.statements[at];

            if( at == chosen.statement )
            {
                deleted[at] = true;
            }
            else if( ( statement.kind == Statement_kind::Storage_dead || statement.kind == Statement_kind::Storage_live ) &&
                     names( statement.place, chosen.gone ) )
            {
                deleted[at] = true;
            }
            else if( statement.kind == Statement_kind::Drop )
            {
                if( ( names( statement.place, chosen.gone ) && !state.initialised[chosen.gone.v] ) ||
                    ( names( statement.place, chosen.kept ) && !state.initialised[chosen.kept.v] ) )
                {
                    deleted[at] = true;
                }
                else if( names( statement.place, chosen.gone ) )
                {
                    gone_drop_kept = true;
                }
            }

            step( func, slots, statement, state );
        }
    }

    return gone_drop_kept;
}

// Merges every candidate that does not clash and shares no local with one already taken; a merge
// changes facts only about its own locals, so one analysis judges them all. Answers whether any merged.
bool coalesce_round( Function& func )
{
    const Slot_map         slots( func );
    std::vector<Occupancy> entry = occupancy_on_entry( func, slots );
    std::vector<Candidate> chosen;
    Local_set              taken( func.locals.size(), false );
    const Liveness         live      = live_locals( func );
    const Local_set        addressed = addressed_locals( func );
    for( u32 index = 0; index < func.statements.size(); ++index )
    {
        const std::optional<Candidate> candidate = candidate_at( func, index );
        if( !candidate.has_value() )
        {
            continue;
        }

        if( taken[candidate->kept.v] || taken[candidate->gone.v] || taken[candidate->into.local.v] )
        {
            continue;
        }

        const bool clash = candidate->into.num_projections == 0
                               ? interferes( func, candidate->kept, candidate->gone, slots, entry, live, addressed )
                               : field_interferes( func, *candidate, slots, entry, live, addressed );
        if( !clash )
        {
            taken[candidate->kept.v]       = true;
            taken[candidate->gone.v]       = true;
            taken[candidate->into.local.v] = true;
            chosen.push_back( *candidate );
        }
    }

    if( chosen.empty() )
    {
        return false;
    }

    // Every mark reads `entry` against the statements as analysed, so all come before any rename.
    std::vector<bool> deleted( func.statements.size(), false );
    std::vector<bool> gone_drop_kept( chosen.size(), false );
    for( u32 c = 0; c < chosen.size(); ++c )
    {
        gone_drop_kept[c] = mark_deletions( func, chosen[c], slots, entry, deleted );
    }

    for( const Candidate& c : chosen )
    {
        rename( func, c.gone, c.into );
    }

    for( u32 s = 0; s < func.statements.size(); ++s )
    {
        if( copies_itself( func, func.statements[s] ) )
        {
            deleted[s] = true;
        }
    }

    remove_statements( func, deleted );

    for( u32 c = 0; c < chosen.size(); ++c )
    {
        // A field merge's locals were refused if either was listed.
        if( chosen[c].into.num_projections != 0 )
        {
            continue;
        }

        // A flag from a move and a drop follows the rename on its own; only a listed `gone` passes its entry on.
        if( std::erase( func.one_path_temporaries, chosen[c].gone ) != 0 && gone_drop_kept[c] &&
            std::ranges::find( func.one_path_temporaries, chosen[c].kept ) == func.one_path_temporaries.end() )
        {
            func.one_path_temporaries.push_back( chosen[c].kept );
        }
    }

    return true;
}

} // namespace

void coalesce_moves( Function& func )
{
    while( coalesce_round( func ) )
    {
        // A round leaves for the next any candidate sharing a local with a merge.
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

constexpr std::string_view k_types = "class Box { public i32 v; public Box( i32 n ) { v = n; } ~Box() { } };\n"
                                     "struct Point { i32 x; i32 y; };\n"
                                     "class Inner { i32 n; public Inner() { n = 0; } ~Inner() { } };\n"
                                     "class Outer { Inner inner; public Outer() { inner = Inner(); } ~Outer() { } };\n"
                                     "Box made( i32 v ) { Box b = Box( v ); return b; }\n"
                                     "Outer outer() { return Outer(); }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it, but coalesced once unless `recoalescing`. The
// function under test is always the first.
struct Optimised : Compiled
{
    explicit Optimised(
        std::string_view              source,
        std::optional<Inline_weights> inlining     = Inline_weights {},
        bool                          splitting    = true,
        u32                           rounds       = 2,
        bool                          recoalescing = false
    )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build {
                  .optimise = true, .inlining = inlining, .splitting = splitting, .rounds = rounds, .recoalescing = recoalescing
              }
          )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    // Whole locals copied or moved into whole locals: what the pass exists to remove.
    std::size_t local_copies() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s )
            {
                const Rvalue& value = first().rvalue( s );

                return s.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Use && value.a.has_place() &&
                       value.a.place.is_whole_local() && s.place.is_whole_local();
            }
        ) );
    }

    std::size_t drops_of( u32 local, bool projected ) const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s )
            {
                return s.kind == Statement_kind::Drop && names( s.place, Local_id { local } ) &&
                       ( s.place.num_projections != 0 ) == projected;
            }
        ) );
    }

    bool well_formed() const
    {
        return std::ranges::all_of( all, []( const Function& f ) { return verify( f ).empty(); } );
    }

    // `into_field`: a whole local copied or moved into a field place; otherwise a field place into a whole local.
    std::size_t field_copies( bool into_field ) const
    {
        const Function& func = first();
        return static_cast<std::size_t>( std::ranges::count_if(
            func.statements,
            [&]( const Statement& s )
            {
                const Rvalue& value = func.rvalue( s );

                if( s.kind != Statement_kind::Assign || value.kind != Rvalue_kind::Use || !value.a.has_place() )
                {
                    return false;
                }

                const Place& whole = into_field ? value.a.place : s.place;
                const Place& field = into_field ? s.place : value.a.place;
                return whole.is_whole_local() && field_place( func, field ) &&
                       !types.table().is_builtin( func.locals[whole.local].type );
            }
        ) );
    }
};

// Rounds `coalesce_moves` takes over `source`'s first function, prepared as a release build
// prepares it, with nothing inlined.
u32 rounds_of( std::string_view source )
{
    Compiled p( std::string( source ) + std::string( k_types ), Stage::Reported );
    REQUIRE( p.clean() );
    Function& func = p.functions.front();
    settle_debug( func, p.literals.add_integer( 0 ) );
    simplify( func, p.literals );
    u32 rounds = 0;
    while( coalesce_round( func ) )
    {
        ++rounds;
    }

    REQUIRE( verify( func ).empty() );
    return rounds;
}

} // namespace

TEST_CASE( "coalesce_moves_builds_in_place", "[opt][coalesce]" )
{
    SECTION( "a local returned is built in the return slot, with no flag" )
    {
        const Optimised p( "Box f( i32 v ) { Box b = Box( v ); return b; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 0 );
        REQUIRE( std::ranges::none_of( p.first().statements, []( const Statement& s ) { return s.drop_flag.is_valid(); } ) );
    }

    SECTION( "a call's result into a local" )
    {
        const Optimised p( "i32 f() { Box b = made( 1 ); return b.v; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 0 );
    }

    SECTION( "a struct literal, through a copy at its last read" )
    {
        const Optimised p( "Point f( i32 v ) { Point p = Point { v, v }; return p; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 0 );
    }

    SECTION( "each arm of a conditional" )
    {
        const Optimised p( "Box f( bool c ) { Box r = c ? Box( 1 ) : Box( 2 ); return r; }\n" );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 0 );
        REQUIRE(
            std::ranges::none_of( p.first().statements, []( const Statement& s ) { return s.kind == Statement_kind::Drop; } )
        );
    }
}

TEST_CASE( "coalesce_moves_keeps_two_values_alive_at_once", "[opt][coalesce]" )
{
    SECTION( "a reassignment builds the new value before the old one drops" )
    {
        const Optimised p( "i32 f() { Box a = Box( 1 ); a = Box( 2 ); return a.v; }\n", Inline_weights {}, false );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 1 );
    }

    SECTION( "a local alive while an early return builds its own value" )
    {
        const Optimised p(
            "Box f( bool c ) { Box b = Box( 3 ); if( c ) { return Box( 4 ); } return b; }\n", Inline_weights {}, false
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 1 );
    }

    // One round, as the next folds `q.x` to `1`.
    SECTION( "a copy read again" )
    {
        const Optimised p(
            "i32 f() { Point p = Point { 1, 2 }; Point q = p; p.x = 5; return q.x; }\n", Inline_weights {}, false, 1
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.local_copies() == 1 );
    }
}

// The scrutinee merges into the parameter; the arm that never moved it drops it once, and the
// parameter's own drop, of a value already moved, goes.
TEST_CASE( "coalesce_moves_drops_a_scrutinee_once", "[opt][coalesce]" )
{
    const Optimised p(
        "i32 f( move Shape s ) { switch( move s ) { case Shape::boxed( b ): return b.v; case Shape::empty: return 0; } }\n"
        "enum Shape { boxed( Box b ), empty };\n"
    );
    REQUIRE( p.clean() );
    REQUIRE( p.well_formed() );
    REQUIRE( p.drops_of( 1, false ) == 1 );
}

// The scrutinee's copy into each arm's binding is its last read, so every arm builds in place.
TEST_CASE( "coalesce_moves_merges_a_scrutinee_in_every_arm", "[opt][coalesce]" )
{
    const Optimised p(
        "i32 f( move Shape s ) { switch( move s ) { case Shape::boxed( b ): return b.v; case Shape::empty: return 0; } }\n"
        "enum Shape { boxed( Box b ), empty };\n"
    );
    REQUIRE( p.clean() );
    REQUIRE( p.well_formed() );
    REQUIRE( p.local_copies() == 0 );
}

// `drop _1` runs `~Outer`'s body and `drop _1.inner` the field after it; neither may go. Not
// inlined, as both bodies are empty.
TEST_CASE( "coalesce_moves_keeps_the_field_drops_after_a_destructor", "[opt][coalesce]" )
{
    const Optimised p( "i32 f() { Outer o = outer(); return 0; }\n", std::nullopt );
    REQUIRE( p.clean() );
    REQUIRE( p.well_formed() );
    REQUIRE( p.local_copies() == 0 );
    REQUIRE( p.drops_of( 1, false ) == 1 );
    REQUIRE( p.drops_of( 1, true ) == 1 );
}

constexpr std::string_view k_shapes = "struct Line { Point a; Point b; };\n"
                                      "enum Shape { boxed( Box b ), paired( Box first, Box second ), empty };\n"
                                      "void consume( move Box b ) { }\n";

TEST_CASE( "coalesce_moves_builds_a_literal_in_its_field", "[opt][coalesce][field]" )
{
    SECTION( "returned" )
    {
        const Optimised p(
            "Line f( i32 v ) { return Line { Point { v, v * 2 }, Point { 3, 4 } }; }\n" + std::string( k_shapes )
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.field_copies( true ) == 0 );
    }

    SECTION( "into a declared local" )
    {
        const Optimised p(
            "i32 f( i32 v ) { Line l = Line { Point { v, 1 }, Point { 2, v } }; return l.a.x; }\n" + std::string( k_shapes )
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.field_copies( true ) == 0 );
    }
}

// Not inlined, so the constructor's call keeps its `&init`.
TEST_CASE( "coalesce_moves_constructs_a_payload_in_place", "[opt][coalesce][field]" )
{
    const Optimised p( "Shape f( i32 v ) { return Shape::boxed( Box( v ) ); }\n" + std::string( k_shapes ), std::nullopt );
    REQUIRE( p.clean() );
    REQUIRE( p.well_formed() );
    REQUIRE( std::ranges::any_of(
        p.first().statements,
        [&]( const Statement& s )
        {
            const Rvalue& value = p.first().rvalue( s );

            return value.kind == Rvalue_kind::Address_of && value.address_purpose == Address_purpose::Initialise &&
                   names( value.a.place, k_return_slot ) && field_place( p.first(), value.a.place );
        }
    ) );
}

// Each binding names its payload where it lies, and each payload drops once. Not inlined, so the
// drops stay to count.
TEST_CASE( "coalesce_moves_binds_a_payload_where_it_lies", "[opt][coalesce][field]" )
{
    const Optimised p(
        "i32 f( move Shape s ) { switch( move s ) { case Shape::boxed( b ): return b.v; "
        "case Shape::paired( first, second ): return first.v + second.v; case Shape::empty: return 0; } }\n" +
            std::string( k_shapes ),
        std::nullopt
    );
    REQUIRE( p.clean() );
    REQUIRE( p.well_formed() );
    REQUIRE( p.field_copies( false ) == 0 );
    REQUIRE( p.drops_of( 1, true ) == 3 );
}

// Where scalar replacement would split the local, it is off, so the kept copy is still a field's.
TEST_CASE( "coalesce_moves_keeps_a_field_copy_that_must_stay", "[opt][coalesce][field]" )
{
    SECTION( "the new value reads the field it replaces" )
    {
        const Optimised p(
            "i32 f() { Line l = Line { Point { 1, 2 }, Point { 3, 4 } }; l.a = Point { l.a.y, l.a.x }; "
            "return l.a.x; }\n" +
                std::string( k_shapes ),
            Inline_weights {},
            false
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.field_copies( true ) == 1 );
    }

    SECTION( "a borrow reads the field" )
    {
        const Optimised p(
            "i32 f() { Line l = Line { Point { 1, 2 }, Point { 3, 4 } }; ref Line r = l; "
            "l.a = Point { r.a.y, 7 }; return l.a.x; }\n" +
                std::string( k_shapes ),
            Inline_weights {},
            false
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.field_copies( true ) == 1 );
    }

    SECTION( "the old value drops after the new one is built" )
    {
        const Optimised p(
            "i32 f() { Holder h = Holder( 1 ); h.b = Box( 2 ); return h.b.v; }\n"
            "class Holder { public Box b; public Holder( i32 n ) { b = Box( n ); } ~Holder() { } };\n",
            Inline_weights {},
            false
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.field_copies( true ) == 1 );
    }

    SECTION( "the copy changes and the field is read after" )
    {
        const Optimised p(
            "i32 f() { Line l = Line { Point { 1, 2 }, Point { 3, 4 } }; Point q = l.a; q.x = 5; "
            "return l.a.x + q.x; }\n" +
                std::string( k_shapes ),
            Inline_weights {},
            false
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.field_copies( false ) == 1 );
    }

    SECTION( "the binding is moved on one path" )
    {
        const Optimised p(
            "i32 f( move Shape s, bool c ) { switch( move s ) { case Shape::boxed( b ): "
            "if( c ) { consume( move b ); } return 1; case Shape::paired( x, y ): return 2; "
            "case Shape::empty: return 0; } }\n" +
            std::string( k_shapes )
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.field_copies( false ) >= 1 );
    }
}

// Splitting leaves `o.a.n`'s rebuilt pointer, a local of its own, reached through copies, which only
// the second coalescing sees.
TEST_CASE( "coalesce_moves_again_builds_a_split_field_in_place", "[opt][coalesce][field]" )
{
    const std::string source =
        "i32 f() { Outer2 o = Outer2 { Inner2 { Cell( 3 ), 0 }, Inner2 { Cell( 4 ), 0 } }; take( move o.a ); "
        "o.a.v = 3; o.a.n = Cell( 5 ); return o.a.n.get() + o.b.n.get(); }\n"
        "class Cell { i32* p; public Cell( i32 v ) { unsafe { p = alloc<i32>(); } *p = v; } "
        "~Cell() { unsafe { free( p ); } } public i32 get() const { return *p; } };\n"
        "class Inner2 { public Cell n; public i32 v; };\n"
        "class Outer2 { public Inner2 a; public Inner2 b; };\n"
        "void take( move Inner2 _ ) { }\n";

    const Optimised once( source );
    const Optimised again( source, Inline_weights {}, true, 2, true );
    REQUIRE( again.clean() );
    REQUIRE( again.well_formed() );
    REQUIRE( once.local_copies() > 0 );
    REQUIRE( again.local_copies() == 0 );
}

TEST_CASE( "coalesce_moves_judges_a_borrow_by_where_it_reaches", "[opt][coalesce][field]" )
{
    SECTION( "borrowed only once built" )
    {
        const Optimised p(
            "i32 f( i32 v ) { Line l = Line { Point { v, 1 }, Point { 2, v } }; ref Line r = l; "
            "return r.a.x; }\n" +
            std::string( k_shapes )
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.field_copies( true ) == 0 );
    }

    SECTION( "borrowed before a loop that builds a literal each turn" )
    {
        const Optimised p(
            "i32 f( i32 n ) { Line l = Line { Point { 0, 0 }, Point { 0, 0 } }; ref Line r = l; i32 t = 0; "
            "for( i32 i = 0; i < n; i++ ) { l.a = Point { i, 1 }; t = t + r.a.x; } return t; }\n" +
                std::string( k_shapes ),
            Inline_weights {},
            false
        );
        REQUIRE( p.clean() );
        REQUIRE( p.well_formed() );
        REQUIRE( p.field_copies( true ) == 1 );
    }
}

// The `err` arm falls through to the scrutinee's drop, so the binding's arm keeps its own local.
// Not inlined: `~Box`'s empty body would leave the binding dead.
TEST_CASE( "coalesce_moves_keeps_a_scrutinee_dropped_after_an_arm_falls_through", "[opt][coalesce]" )
{
    const Optimised p(
        "i32 f( move Shape s ) { switch( move s ) { case Shape::boxed( b ): break; "
        "case Shape::paired( x, y ): break; case Shape::empty: break; } return 0; }\n" +
            std::string( k_shapes ),
        std::nullopt
    );
    REQUIRE( p.clean() );
    REQUIRE( p.well_formed() );
    REQUIRE( p.local_copies() >= 1 );
}

// `code` merges into the return slot, so each `return code` would be `_0 = copy _0`.
TEST_CASE( "coalesce_moves_leaves_no_self_copy", "[opt][coalesce]" )
{
    const Optimised p(
        "i32 f() { i32 code = g(); if( code != 0 ) { return code; } code = g(); if( code != 0 ) { return code; } "
        "return 0; }\n"
        "i32 g() { return 1; }\n"
    );
    REQUIRE( p.clean() );
    REQUIRE( p.well_formed() );
    REQUIRE( std::ranges::none_of( p.first().statements, [&]( const Statement& s ) { return copies_itself( p.first(), s ); } )
    );
}

TEST_CASE( "coalesce_moves_merges_independent_moves_in_one_round", "[opt][coalesce]" )
{
    const u32 one   = rounds_of( "i32 f( move Box x ) { Box b = move x; return b.v; }\n" );
    const u32 two   = rounds_of( "i32 f( move Box x, move Box y ) { Box b = move x; Box c = move y; return b.v + c.v; }\n" );
    const u32 chain = rounds_of( "i32 f( move Box x ) { Box b = move x; Box c = move b; return c.v; }\n" );

    // Independent pairs share a round; each link of a chain waits for the one before.
    REQUIRE( one == 1 );
    REQUIRE( two == 1 );
    REQUIRE( chain == 2 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
