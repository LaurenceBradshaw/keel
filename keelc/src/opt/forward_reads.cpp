// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/forward_reads.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>
#include "ir/initialised.h"
#include "ir/simplify.h"

namespace keel
{

bool mentions( const Function& func, const Place& place, Local_id local )
{
    if( !place.is_global() && place.local == local )
    {
        return true;
    }

    return std::ranges::any_of(
        func.projections_of( place ),
        [&]( const Projection& projection ) { return projection.kind == Projection_kind::Index && projection.index == local; }
    );
}

bool reads( const Function& func, const Statement& statement, Local_id local )
{
    if( statement.kind == Statement_kind::Drop )
    {
        return statement.drop_flag == local || mentions( func, statement.place, local );
    }
    if( !statement.place.is_whole_local() && mentions( func, statement.place, local ) )
    {
        return true;
    }
    if( !statement.value.is_valid() )
    {
        return false;
    }

    bool read = false;
    for_each_operand(
        func.rvalue( statement ),
        [&]( const Operand& operand ) { read |= operand.has_place() && mentions( func, operand.place, local ); }
    );
    return read;
}

bool reads( const Function& func, const Terminator& terminator, Local_id local )
{
    bool read = false;
    for( const Operand* operand : { &terminator.condition, &terminator.message, &terminator.at } )
    {
        read |= operand->has_place() && mentions( func, operand->place, local );
    }
    return read;
}

Local_set addressed_locals_of( const Function& func )
{
    Local_set addressed( func.locals.size(), false );
    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        const Rvalue& value = func.rvalue( statement );
        if( value.kind == Rvalue_kind::Address_of && value.address_purpose != Address_purpose::Initialise &&
            !value.a.place.is_global() && !has_deref_projection( func, value.a.place ) )
        {
            addressed[value.a.place.local.v] = true;
        }
    }

    return addressed;
}

bool is_addressed( const Local_set& addressed, Local_id local )
{
    return local.v < addressed.size() && addressed[local.v];
}

bool clobbers( const Function& func, const Local_set& addressed, const Statement& statement )
{
    if( statement.kind == Statement_kind::Drop )
    {
        return true;
    }

    if( statement.kind != Statement_kind::Assign )
    {
        return false;
    }

    const Rvalue_kind kind = func.rvalue( statement ).kind;
    if( kind == Rvalue_kind::Call || kind == Rvalue_kind::Indirect_call || kind == Rvalue_kind::Allocate ||
        kind == Rvalue_kind::Release )
    {
        return true;
    }

    const Place& place = statement.place;
    return place.is_global() || has_deref_projection( func, place ) || is_addressed( addressed, place.local );
}

namespace
{
// A place read through a pointer, and the local holding what it read. A place without a deref is
// propagate_values's, which forwards the other way.
using Reads = std::vector<std::pair<Place, Local_id>>;

void forget( const Function& func, Reads& reads, Local_id local )
{
    std::erase_if(
        reads,
        [&]( const std::pair<Place, Local_id>& read ) { return read.second == local || mentions( func, read.first, local ); }
    );
}

const Local_id* held( const Function& func, const Reads& reads, const Place& place )
{
    for( const std::pair<Place, Local_id>& read : reads )
    {
        if( same_place( func, read.first, place ) )
        {
            return &read.second;
        }
    }

    return nullptr;
}

void step( const Function& func, const Local_set& addressed, const Statement& statement, Reads& reads )
{
    if( clobbers( func, addressed, statement ) )
    {
        reads.clear();
    }
    else if( !statement.place.is_global() && statement.place.local.is_valid() )
    {
        forget( func, reads, statement.place.local );
    }

    if( statement.kind != Statement_kind::Assign || !statement.place.is_whole_local() )
    {
        return;
    }

    const Rvalue&  value = func.rvalue( statement );
    const Operand& from  = value.a;
    const Local_id into  = statement.place.local;

    if( value.kind == Rvalue_kind::Use && from.kind == Operand_kind::Copy && has_deref_projection( func, from.place ) &&
        from.type == func.locals[into].type && !mentions( func, from.place, into ) && !held( func, reads, from.place ) )
    {
        reads.emplace_back( from.place, into );
    }
}

std::vector<std::optional<Reads>> reads_on_entry( const Function& func, const Local_set& addressed )
{
    std::vector<std::optional<Reads>> entry( func.blocks.size(), std::nullopt );
    entry[0].emplace();

    // Reads only go away, and a block is reached once, so this ends.
    bool                  changed = true;
    std::vector<Block_id> targets;
    std::vector<bool>     dirty( func.blocks.size(), true );
    Reads                 reads;
    while( changed )
    {
        changed = false;

        for( const Block_id b : func.blocks.ids() )
        {
            if( !entry[b.v] || !dirty[b.v] )
            {
                continue;
            }
            dirty[b.v] = false;

            const Block& block = func.blocks[b];
            reads              = *entry[b.v];
            for( const Statement& statement : func.statements_of( block ) )
            {
                step( func, addressed, statement, reads );
            }

            targets.clear();
            successors( block.terminator, targets );
            for( const Block_id target : targets )
            {
                std::optional<Reads>& into = entry[target.v];
                if( !into )
                {
                    into            = reads;
                    changed         = true;
                    dirty[target.v] = true;
                    continue;
                }

                // A read survives a join only if this path holds it in the same local.
                const std::size_t before = into->size();
                std::erase_if(
                    *into,
                    [&]( const std::pair<Place, Local_id>& read )
                    {
                        const Local_id* local = held( func, reads, read.first );
                        return !local || *local != read.second;
                    }
                );
                if( into->size() != before )
                {
                    changed         = true;
                    dirty[target.v] = true;
                }
            }
        }
    }

    return entry;
}

bool rewrite( const Function& func, const Reads& reads, Operand& operand )
{
    if( operand.kind != Operand_kind::Copy || !has_deref_projection( func, operand.place ) )
    {
        return false;
    }

    const Local_id* local = held( func, reads, operand.place );
    if( !local || func.locals[*local].type != operand.type )
    {
        return false;
    }

    operand = copy( Place { .local = *local }, operand.type );
    return true;
}
} // namespace

bool forward_reads( Function& func )
{
    const Local_set                         addressed = addressed_locals_of( func );
    const std::vector<std::optional<Reads>> entry     = reads_on_entry( func, addressed );
    bool                                    changed   = false;
    std::vector<bool>                       deleted( func.statements.size(), false );

    for( const Block_id b : func.blocks.ids() )
    {
        if( !entry[b.v] )
        {
            continue;
        }

        Block& block = func.blocks[b];
        Reads  reads = *entry[b.v];

        for( u32 s = 0; s < block.statement_count; ++s )
        {
            Statement& statement = func.statements_of( block )[s];

            // Reads first, so `_4 = copy (*_1).n` reads the old `_4`. An address names its place, not a read.
            if( statement.kind == Statement_kind::Assign && func.rvalue( statement ).kind != Rvalue_kind::Address_of )
            {
                Rvalue& value = func.rvalue( statement );
                changed |= rewrite( func, reads, value.a );
                changed |= rewrite( func, reads, value.b );
                for( Operand& argument : value.arguments )
                {
                    changed |= rewrite( func, reads, argument );
                }

                // A latch's `_4 = copy (*_1).n` the guard already read is now `_4 = copy _4`.
                if( value.kind == Rvalue_kind::Use && value.a.kind == Operand_kind::Copy && statement.place.is_whole_local() &&
                    value.a.place.is_whole_local() && value.a.place.local == statement.place.local )
                {
                    deleted[block.first_statement + s] = true;
                    continue;
                }
            }

            step( func, addressed, statement, reads );
        }

        changed |= rewrite( func, reads, block.terminator.condition );
        changed |= rewrite( func, reads, block.terminator.message );
        changed |= rewrite( func, reads, block.terminator.at );
    }

    if( changed )
    {
        remove_statements( func, deleted );
    }

    return changed;
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

constexpr std::string_view k_types = "struct Pair { i32 a; i32 b; i32 size() const { return a; } };\n"
                                     "i32 g = 0;\n"
                                     "void touch() { g += 1; }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Forwarded : Compiled
{
    explicit Forwarded( std::string_view source, u32 level = 3, std::optional<Loop_weights> loops = Loop_weights {} )
        : Compiled(
              std::string( source ) + std::string( k_types ), Stage::Elaborated, Build { .level = level, .loops = loops }
          )
    {
        INFO( rendered() );
        REQUIRE( clean() );
        REQUIRE( verify( functions.front() ).empty() );
    }

    std::size_t branches() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            functions.front().blocks, []( const Block& block ) { return block.terminator.kind == Terminator_kind::Branch; }
        ) );
    }
};

constexpr std::string_view k_checked =
    "i32 f( const ref Pair p, i32 i ) { if( i < p.size() ) { if( i >= p.a ) { return -1; } return 1; } return 0; }\n";

} // namespace

// The second read of `p.a` takes the size's local, so the inner test is the outer's negation.
TEST_CASE( "forward_reads_folds_a_test_a_read_through_a_reference_repeats", "[opt][forward_reads]" )
{
    REQUIRE( Forwarded( k_checked ).branches() == 1 );
    REQUIRE( Forwarded( k_checked, 3, std::nullopt ).branches() == 2 );
}

// `touch` writes a global, which `p` may reach.
TEST_CASE( "forward_reads_keeps_a_read_a_global_store_separates", "[opt][forward_reads]" )
{
    const Forwarded f( "i32 f( const ref Pair p, i32 i ) { if( i < p.size() ) { touch(); if( i >= p.a ) { return -1; } return "
                       "1; } return 0; }\n" );
    REQUIRE( f.branches() == 2 );
}

// `q` may be `p`.
TEST_CASE( "forward_reads_keeps_a_read_a_store_through_a_pointer_separates", "[opt][forward_reads]" )
{
    const Forwarded f( "i32 f( const ref Pair p, ref Pair q, i32 i ) { if( i < p.size() ) { q.a = 0; if( i >= p.a ) { "
                       "return -1; } return 1; } return 0; }\n" );
    REQUIRE( f.branches() == 2 );
}

// One path into the inner test stores through `q`, so the join keeps the read.
TEST_CASE( "forward_reads_keeps_a_read_one_path_stores_before", "[opt][forward_reads]" )
{
    const Forwarded f( "i32 f( const ref Pair p, ref Pair q, i32 i, bool c ) { if( i < p.size() ) { if( c ) { q.b = 0; } "
                       "if( i >= p.a ) { return -1; } return 1; } return 0; }\n" );
    REQUIRE( f.branches() == 3 );
}

// `r` may point at `x`, so a store to `x` reaches `(*r).a`.
TEST_CASE( "forward_reads_keeps_a_read_a_store_to_an_addressed_local_separates", "[opt][forward_reads]" )
{
    const Forwarded f( "i32 f( i32 i, i32 k, bool c ) { Pair x = Pair { k, 0 }; Pair y = Pair { k, 1 }; Pair* r = &x; "
                       "if( c ) { r = &y; } if( i < r.size() ) { x.a = 0; if( i >= r.a ) { return -1; } return 1; } "
                       "return 0; }\n" );
    REQUIRE( f.branches() == 3 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
