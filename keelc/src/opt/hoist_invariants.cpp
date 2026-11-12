// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/hoist_invariants.h"
#include <algorithm>
#include <utility>
#include <vector>
#include "ir/loops.h"
#include "ir/simplify.h"
#include "opt/forward_reads.h"

namespace keel
{

namespace
{
// One loop, as the function now stands.
struct Scope
{
    const Function&       func;
    const Loops&          found;
    const Loop&           loop;
    const Local_set&      addressed;
    std::vector<bool>     own;     // by block: in the loop and in none inside it
    std::vector<u32>      writes;  // by local: statements in the loop that write it or take its address
    std::vector<Block_id> exiting; // blocks in the loop with an edge out
    bool                  clobbered = false;

    Scope( const Function& f, const Loops& l, u32 index, const std::vector<u32>& innermost, const Local_set& a )
        : func( f ),
          found( l ),
          loop( l.loops[index] ),
          addressed( a ),
          own( f.blocks.size(), false ),
          writes( f.locals.size(), 0 )
    {
        std::vector<Block_id> targets;
        for( const Block_id b : loop.blocks )
        {
            own[b.v] = innermost[b.v] == index;
            for( const Statement& statement : func.statements_of( b ) )
            {
                clobbered |= clobbers( func, addressed, statement );
                if( !statement.place.is_global() && statement.place.local.is_valid() )
                {
                    ++writes[statement.place.local.v];
                }
                if( statement.kind == Statement_kind::Assign )
                {
                    const Rvalue& value = func.rvalue( statement );
                    if( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() &&
                        !has_deref_projection( func, value.a.place ) )
                    {
                        writes[value.a.place.local.v] += 2;
                    }
                }
            }

            targets.clear();
            successors( func.blocks[b].terminator, targets );
            for( const Block_id target : targets )
            {
                if( !loop.contains( target ) )
                {
                    exiting.push_back( b );
                    break;
                }
            }
        }
    }

    bool invariant( Local_id local ) const
    {
        return writes[local.v] == 0 && !( is_addressed( addressed, local ) && clobbered );
    }

    bool invariant( const Place& place ) const
    {
        if( place.is_global() ? clobbered : !invariant( place.local ) )
        {
            return false;
        }
        for( const Projection& projection : func.projections_of( place ) )
        {
            if( projection.kind == Projection_kind::Index && !invariant( projection.index ) )
            {
                return false;
            }
        }
        return !clobbered || !has_deref_projection( func, place );
    }

    bool invariant( const Operand& operand ) const
    {
        return operand.kind == Operand_kind::Constant || operand.kind == Operand_kind::Debug ||
               ( operand.kind == Operand_kind::Copy && invariant( operand.place ) );
    }

    bool invariant( const Rvalue& value ) const
    {
        if( value.kind == Rvalue_kind::Address_of )
        {
            return value.address_purpose != Address_purpose::Initialise && has_deref_projection( func, value.a.place ) &&
                   invariant( value.a.place );
        }

        switch( value.kind )
        {
        case Rvalue_kind::Use:
        case Rvalue_kind::Binary:
        case Rvalue_kind::Unary:
        case Rvalue_kind::Overflows:
        case Rvalue_kind::Cast:
        case Rvalue_kind::Bits:
        case Rvalue_kind::Run_of:
        case Rvalue_kind::Run_data:
        case Rvalue_kind::Run_size:
            break;
        default:
            return false;
        }

        bool all = true;
        for_each_operand( value, [&]( const Operand& operand ) { all &= invariant( operand ); } );
        return all;
    }

    // Whether the statement at `index` in `block` runs on the loop's first turn, with nothing a
    // program could see before it: so a read through a pointer, or a panic, may run in the preheader.
    bool runs_unobserved( Block_id block, u32 index ) const
    {
        for( const Block_id exit : exiting )
        {
            if( !found.dominators.dominates( block, exit ) )
            {
                return false;
            }
        }

        const Run<const Statement> statements = func.statements_of( block );
        for( u32 s = 0; s < index; ++s )
        {
            if( clobbers( func, addressed, statements[s] ) )
            {
                return false;
            }
        }

        // What runs before it on the first turn, the header back.
        std::vector<bool>     seen( func.blocks.size(), false );
        std::vector<Block_id> work { block };
        std::vector<Block_id> targets;
        seen[block.v] = true;
        while( !work.empty() )
        {
            const Block_id b = work.back();
            work.pop_back();
            if( b != block )
            {
                // An inner loop might not end.
                if( !own[b.v] )
                {
                    return false;
                }
                for( const Statement& statement : func.statements_of( b ) )
                {
                    if( clobbers( func, addressed, statement ) )
                    {
                        return false;
                    }
                }
            }
            if( b == loop.header )
            {
                continue;
            }
            for( const Block_id p : loop.blocks )
            {
                targets.clear();
                successors( func.blocks[p].terminator, targets );
                if( !seen[p.v] && std::ranges::find( targets, b ) != targets.end() )
                {
                    seen[p.v] = true;
                    work.push_back( p );
                }
            }
        }
        return true;
    }
};

bool traps( const Function& func, const Rvalue& value )
{
    if( value.kind == Rvalue_kind::Address_of )
    {
        return false;
    }
    if( value.kind == Rvalue_kind::Cast ||
        ( value.kind == Rvalue_kind::Binary && ( value.op == Token_kind::Slash || value.op == Token_kind::Percent ) ) )
    {
        return true;
    }

    bool through = false;
    for_each_operand(
        value,
        [&]( const Operand& operand )
        { through |= operand.kind == Operand_kind::Copy && has_deref_projection( func, operand.place ); }
    );
    return through;
}

// Whether `statement` or `terminator` reads `local`; a write of the whole local is not a read, and a
// drop reads its place and its flag.
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

// Whether `local`, written by the statement at `index` in `block`, is read in the loop only after it,
// and, unless that statement runs on every entry, not read outside it.
bool read_only_after( const Scope& scope, Block_id block, u32 index, Local_id local, bool every_entry )
{
    const Function& func = scope.func;
    // The return slot is read by a return, which names no operand.
    if( !every_entry && local.v == 0 )
    {
        return false;
    }
    for( const Block_id b : func.blocks.ids() )
    {
        const bool inside = scope.loop.contains( b );
        if( !inside && every_entry )
        {
            continue;
        }

        const bool                 after      = inside && b != block && scope.found.dominators.dominates( block, b );
        const Run<const Statement> statements = func.statements_of( b );
        for( u32 s = 0; s < statements.size(); ++s )
        {
            if( reads( func, statements[s], local ) && !after && !( b == block && s > index ) )
            {
                return false;
            }
        }
        if( reads( func, func.blocks[b].terminator, local ) && !after && b != block )
        {
            return false;
        }
    }
    return true;
}

// `statement` appended to `block`'s statements, which move to the end.
void append( Function& func, Block_id block, Statement statement )
{
    std::vector<Statement> moved;
    for( Statement& s : func.statements_of( block ) )
    {
        moved.push_back( std::move( s ) );
    }
    moved.push_back( std::move( statement ) );

    func.blocks[block].first_statement = narrow_cast<u32>( func.statements.size() );
    func.blocks[block].statement_count = narrow_cast<u32>( moved.size() );
    for( Statement& s : moved )
    {
        func.statements.push_back( std::move( s ) );
    }
}

// Moves one invariant statement or read to the preheader; whether it found one.
bool hoist_one( Function& func, const Scope& scope )
{
    const Block_id preheader = scope.loop.preheader;
    for( const Block_id b : scope.loop.blocks )
    {
        if( !scope.own[b.v] )
        {
            continue;
        }

        Block& block = func.blocks[b];
        for( u32 s = 0; s <= block.statement_count; ++s )
        {
            const bool at_end    = s == block.statement_count;
            Statement* statement = at_end ? nullptr : &func.statements[block.first_statement + s];

            if( statement && statement->kind == Statement_kind::Assign && statement->place.is_whole_local() )
            {
                const Rvalue&  value = func.rvalue( *statement );
                const Local_id local = statement->place.local;
                if( scope.writes[local.v] == 1 && !is_addressed( scope.addressed, local ) && scope.invariant( value ) )
                {
                    const bool every_entry = scope.runs_unobserved( b, s );
                    if( ( every_entry || !traps( func, value ) ) && read_only_after( scope, b, s, local, every_entry ) )
                    {
                        const u32 index = block.first_statement + s;
                        append( func, preheader, std::move( func.statements[index] ) );
                        std::vector<bool> deleted( func.statements.size(), false );
                        deleted[index] = true;
                        remove_statements( func, deleted );
                        return true;
                    }
                }
            }

            // A read through a pointer inside a statement that stays.
            std::vector<Operand*> operands;
            if( at_end )
            {
                operands = { &block.terminator.condition, &block.terminator.message, &block.terminator.at };
            }
            else if( statement->kind == Statement_kind::Assign && func.rvalue( *statement ).kind != Rvalue_kind::Address_of )
            {
                Rvalue& value = func.rvalue( *statement );
                operands      = { &value.a, &value.b };
                for( Operand& argument : value.arguments )
                {
                    operands.push_back( &argument );
                }
            }

            for( Operand* operand : operands )
            {
                if( operand->kind != Operand_kind::Copy || !has_deref_projection( func, operand->place ) ||
                    !scope.invariant( operand->place ) || !scope.runs_unobserved( b, s ) )
                {
                    continue;
                }

                // Before add_rvalue, which can move the rvalue `operand` lives in.
                const Operand  from  = *operand;
                const Span     span  = at_end ? block.terminator.span : statement->span;
                const Local_id local = func.locals.push_back( Local { .type = from.type, .span = span } );
                *operand             = copy( Place { .local = local }, from.type );
                Statement read {
                    .span  = span,
                    .place = Place { .local = local },
                    .value = func.add_rvalue( Rvalue { .kind = Rvalue_kind::Use, .type = from.type, .a = from } )
                };
                append( func, preheader, std::move( read ) );
                remove_statements( func, std::vector<bool>( func.statements.size(), false ) );
                return true;
            }
        }
    }
    return false;
}
} // namespace

bool hoist_invariants( Function& func )
{
    // The round before merged the preheaders away.
    bool            changed   = normalise_loops( func );
    const Loops     found     = find_loops( func );
    const Local_set addressed = addressed_locals_of( func );

    std::vector<u32> innermost( func.blocks.size(), k_no_loop );
    for( u32 l = 0; l < found.loops.size(); ++l )
    {
        for( const Block_id b : found.loops[l].blocks )
        {
            if( innermost[b.v] == k_no_loop )
            {
                innermost[b.v] = l;
            }
        }
    }

    for( u32 l = 0; l < found.loops.size(); ++l )
    {
        if( !found.loops[l].preheader.is_valid() )
        {
            continue;
        }
        while( hoist_one( func, Scope( func, found, l, innermost, addressed ) ) )
        {
            changed = true;
        }
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

constexpr std::string_view k_types = "struct Pair { i32 a; i32 b; };\n"
                                     "i32 g = 0;\n"
                                     "void touch() { g += 1; }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Hoisted : Compiled
{
    explicit Hoisted( std::string_view source, u32 level = 3 )
        : Compiled( std::string( source ) + std::string( k_types ), Stage::Elaborated, Build { .level = level } )
    {
        INFO( rendered() );
        REQUIRE( clean() );
        REQUIRE( verify( functions.front() ).empty() );
    }

    // Statements in a loop that `fn` picks.
    template <typename Fn>
    std::size_t in_loops( Fn fn ) const
    {
        const Function&   func = functions.front();
        std::vector<bool> inside( func.blocks.size(), false );
        for( const Loop& loop : find_loops( func ).loops )
        {
            for( const Block_id b : loop.blocks )
            {
                inside[b.v] = true;
            }
        }

        std::size_t count = 0;
        for( const Block_id b : func.blocks.ids() )
        {
            for( const Statement& statement : func.statements_of( b ) )
            {
                count += inside[b.v] && statement.kind == Statement_kind::Assign && fn( func, func.rvalue( statement ) );
            }
        }
        return count;
    }

    std::size_t reads_through() const
    {
        return in_loops(
            []( const Function& func, const Rvalue& value )
            {
                bool through = false;
                for_each_operand(
                    value,
                    [&]( const Operand& operand )
                    { through |= operand.kind == Operand_kind::Copy && has_deref_projection( func, operand.place ); }
                );
                return through;
            }
        );
    }

    std::size_t by( Token_kind op ) const
    {
        return in_loops(
            [&]( const Function&, const Rvalue& value )
            { return value.kind == Rvalue_kind::Binary && value.op == op && value.b.kind == Operand_kind::Constant; }
        );
    }
};

} // namespace

TEST_CASE( "hoist_invariants_hoists_a_read_through_a_reference_and_a_product", "[opt][hoist_invariants]" )
{
    constexpr std::string_view source = "i64 f( const ref Pair p, i64 n, i64 k ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) "
                                        "{ t += i * p.a * ( k * 4 ); } return t; }\n";
    const Hoisted              f( source );
    REQUIRE( f.reads_through() == 0 );
    REQUIRE( f.by( Token_kind::Star ) == 0 );

    const Hoisted o2( source, 2 );
    REQUIRE( o2.reads_through() == 1 );
    REQUIRE( o2.by( Token_kind::Star ) == 1 );
}

// `touch` writes a global, which `p` may reach, so the next turn's read sees what it wrote.
TEST_CASE( "hoist_invariants_keeps_a_read_a_call_follows", "[opt][hoist_invariants]" )
{
    const Hoisted f( "i64 f( const ref Pair p, i64 n ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { t += p.a; touch(); } "
                     "return t; }\n" );
    REQUIRE( f.reads_through() == 1 );
}

// A test of two parts is not rotated, so the body may not run, and `p.a` stays in it.
TEST_CASE( "hoist_invariants_keeps_a_read_an_unrotated_loop_may_skip", "[opt][hoist_invariants]" )
{
    const Hoisted f( "i64 f( const ref Pair p, i64 n, bool c ) { i64 t = 0; for( i64 i = 0; i < n && c; i += 1 ) { t += p.a; } "
                     "return t; }\n" );
    REQUIRE( f.reads_through() == 1 );
}

// A division may panic, so it waits for the call before it; without the call it is hoisted.
TEST_CASE( "hoist_invariants_keeps_a_division_a_call_runs_before", "[opt][hoist_invariants]" )
{
    const Hoisted kept( "i64 f( i64 n, i64 k ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { touch(); t += k / 3; } "
                        "return t; }\n" );
    REQUIRE( kept.by( Token_kind::Slash ) == 1 );

    const Hoisted hoisted( "i64 f( i64 n, i64 k ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { t += k / 3; } return t; }\n" );
    REQUIRE( hoisted.by( Token_kind::Slash ) == 0 );
}

// The first turn reads `x`'s value from before the loop.
TEST_CASE( "hoist_invariants_keeps_a_write_a_read_comes_before", "[opt][hoist_invariants]" )
{
    const Hoisted f( "i64 f( i64 n, i64 k ) { i64 t = 0; i64 x = 0; for( i64 i = 0; i < n; i += 1 ) { t += x; x = k * 4; } "
                     "return t; }\n" );
    REQUIRE( f.by( Token_kind::Star ) == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
