// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/hoist_invariants.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>
#include "ir/loops.h"
#include "ir/simplify.h"
#include "opt/forward_reads.h"
#include "opt/reduce_inductions.h"

namespace keel
{

namespace
{
// `(*p)` then only fields and elements: a place in the one object `p` points at.
bool through_one( const Function& func, const Place& place )
{
    const Run<const Projection> projections = func.projections_of( place );
    if( place.is_global() || projections.size() == 0 || projections[0].kind != Projection_kind::Deref )
    {
        return false;
    }
    for( u32 i = 1; i < projections.size(); ++i )
    {
        if( projections[i].kind == Projection_kind::Deref )
        {
            return false;
        }
    }
    return true;
}

// Where a pointer local's values all came from.
struct Origin
{
    enum Kind : u8
    {
        Unknown,
        Fresh, // an Allocate in this function, assigned to `fresh`
        Before // a parameter, or a local's address: memory no Allocate here returns
    };
    Kind     kind = Unknown;
    Local_id fresh {};
};

// What a program may see before a panic: a call, drop or release. A store to memory is not, as nothing
// reads memory after one; nor is a pure call that surely returns.
bool observed( const Function& func, const Purity* purity, const Statement& statement )
{
    if( statement.kind == Statement_kind::Drop )
    {
        return true;
    }
    if( statement.kind != Statement_kind::Assign )
    {
        return false;
    }
    const Rvalue&     value = func.rvalue( statement );
    const Rvalue_kind kind  = value.kind;
    if( kind == Rvalue_kind::Call && purity != nullptr && purity->removable( value ) )
    {
        return false;
    }
    return kind == Rvalue_kind::Call || kind == Rvalue_kind::Indirect_call || kind == Rvalue_kind::Release;
}

// One loop, as the function now stands.
struct Scope
{
    const Function&                         func;
    const Type_table&                       table;
    const Loops&                            found;
    const Loop&                             loop;
    const Local_set&                        addressed;
    const Purity*                           purity;  // none: every call may write anything
    std::vector<bool>                       own;     // by block: in the loop and in none inside it
    std::vector<u32>                        writes;  // by local: statements in the loop that write it or take its address
    std::vector<Block_id>                   exiting; // blocks in the loop with an edge out
    std::vector<u32>                        sets;    // by local: the same over the whole function
    std::vector<std::vector<const Rvalue*>> sources; // by local: the rvalues assigned to all of it
    std::vector<Place>                      stores;  // stores in the loop through a pointer
    std::vector<Type_id>                    stored;  // what each of `stores` writes
    bool everything = false;                         // a call, drop, release, or store to a global or an addressed local
    bool anywhere   = false;                         // a store through a pointer that may reach memory from before it

    Scope(
        const Function&         f,
        const Type_table&       t,
        const Loops&            l,
        u32                     index,
        const std::vector<u32>& innermost,
        const Local_set&        a,
        const Purity*           p
    )
        : func( f ),
          table( t ),
          found( l ),
          loop( l.loops[index] ),
          addressed( a ),
          purity( p ),
          own( f.blocks.size(), false ),
          writes( f.locals.size(), 0 ),
          sets( f.locals.size(), 0 ),
          sources( f.locals.size() )
    {
        for( const Block_id b : func.blocks.ids() )
        {
            for( const Statement& statement : func.statements_of( b ) )
            {
                // A storage marker changes no value a later read may see.
                if( statement.kind == Statement_kind::Storage_live || statement.kind == Statement_kind::Storage_dead )
                {
                    continue;
                }
                count( statement, sets );
                if( statement.kind == Statement_kind::Assign && statement.place.is_whole_local() )
                {
                    sources[statement.place.local.v].push_back( &func.rvalue( statement ) );
                }
            }
        }

        std::vector<Block_id> targets;
        for( const Block_id b : loop.blocks )
        {
            own[b.v] = innermost[b.v] == index;
            for( const Statement& statement : func.statements_of( b ) )
            {
                count( statement, writes );
                // A pure call writes only its result.
                const bool pure        = statement.kind == Statement_kind::Assign && pure_call( func.rvalue( statement ) );
                const bool into_memory = statement.place.is_global() || has_deref_projection( func, statement.place ) ||
                                         is_addressed( addressed, statement.place.local );
                if( !clobbers( func, addressed, statement ) || ( pure && !into_memory ) )
                {
                    continue;
                }
                const Rvalue_kind kind =
                    statement.kind == Statement_kind::Assign ? func.rvalue( statement ).kind : Rvalue_kind::Call;
                if( ( !pure &&
                      ( kind == Rvalue_kind::Call || kind == Rvalue_kind::Indirect_call || kind == Rvalue_kind::Release ) ) ||
                    !has_deref_projection( func, statement.place ) )
                {
                    everything = true;
                }
                else
                {
                    stores.push_back( statement.place );
                    stored.push_back( func.rvalue( statement ).type );
                    anywhere |= !through_one( func, statement.place ) || origin( statement.place.local ).kind != Origin::Fresh;
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

    bool pure_call( const Rvalue& value ) const
    {
        return value.kind == Rvalue_kind::Call && purity != nullptr && purity->pure( value );
    }

    // A write of `statement`'s local, or of a local whose address it takes, into `counts`. A store
    // through `*p` writes no part of `p`.
    void count( const Statement& statement, std::vector<u32>& counts ) const
    {
        if( !statement.place.is_global() && statement.place.local.is_valid() && !has_deref_projection( func, statement.place ) )
        {
            ++counts[statement.place.local.v];
        }
        if( statement.kind == Statement_kind::Assign )
        {
            const Rvalue& value = func.rvalue( statement );
            if( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() &&
                !has_deref_projection( func, value.a.place ) )
            {
                counts[value.a.place.local.v] += 2;
            }
        }
    }

    Origin origin( Local_id local ) const
    {
        std::vector<bool> visiting( func.locals.size(), false );
        return origin( local, visiting ).value_or( Origin {} );
    }

    // Every value `local` is assigned, through copies, offsets and addresses within one object; none
    // when `local` is in a cycle of these already being followed, which adds no value of its own.
    std::optional<Origin> origin( Local_id local, std::vector<bool>& visiting ) const
    {
        if( local.v >= sets.size() || is_addressed( addressed, local ) )
        {
            return Origin {};
        }
        if( visiting[local.v] )
        {
            return std::nullopt;
        }
        if( sets[local.v] == 0 )
        {
            return Origin { .kind = local.v >= 1 && local.v <= func.parameter_count ? Origin::Before : Origin::Unknown };
        }
        // Written some other way too: in part, by a drop, or through its address.
        if( sets[local.v] != sources[local.v].size() )
        {
            return Origin {};
        }

        visiting[local.v] = true;
        std::optional<Origin> all;
        for( const Rvalue* value : sources[local.v] )
        {
            std::optional<Origin> one = Origin {};
            if( value->kind == Rvalue_kind::Allocate )
            {
                one = Origin { .kind = Origin::Fresh, .fresh = local };
            }
            else if( value->kind == Rvalue_kind::Address_of )
            {
                if( value->a.place.is_global() || !has_deref_projection( func, value->a.place ) )
                {
                    one = Origin { .kind = Origin::Before };
                }
                else if( through_one( func, value->a.place ) )
                {
                    one = origin( value->a.place.local, visiting );
                }
            }
            // The checker puts a pointer on the left of `+` and `-`, so `a` stays a pointer.
            else if( ( value->kind == Rvalue_kind::Use ||
                       ( value->kind == Rvalue_kind::Binary &&
                         ( value->op == Token_kind::Plus || value->op == Token_kind::Minus ) ) ) &&
                     value->a.kind == Operand_kind::Copy && value->a.place.is_whole_local() )
            {
                one = origin( value->a.place.local, visiting );
            }

            if( !one )
            {
                continue;
            }
            if( one->kind == Origin::Unknown || ( all && ( all->kind != one->kind || all->fresh != one->fresh ) ) )
            {
                all = Origin {};
                break;
            }
            all = one;
        }
        visiting[local.v] = false;
        return all;
    }

    // Whether a store of `stored_type` to `store`, through a pointer, may change `read`, read through one
    // as `read_type`.
    bool reaches( const Place& store, Type_id stored_type, const Place& read, Type_id read_type ) const
    {
        if( distinct_memory( table, stored_type, read_type ) )
        {
            return false;
        }
        if( !through_one( func, store ) || !through_one( func, read ) )
        {
            return true;
        }

        // The same pointer, then different fields of one struct.
        if( store.local == read.local )
        {
            const Run<const Projection> a = func.projections_of( store );
            const Run<const Projection> b = func.projections_of( read );
            for( u32 i = 1; i < std::min( a.size(), b.size() ); ++i )
            {
                if( !same_projection( a[i], b[i] ) )
                {
                    return a[i].kind != Projection_kind::Field || b[i].kind != Projection_kind::Field;
                }
            }
            return true;
        }

        // New memory, and a read of memory that was there before it or came from another Allocate.
        const Origin to   = origin( store.local );
        const Origin from = origin( read.local );
        return to.kind != Origin::Fresh || from.kind == Origin::Unknown ||
               ( from.kind == Origin::Fresh && from.fresh == to.fresh );
    }

    // `read_type` unknown, any store may reach it.
    bool reached( const Place& read, Type_id read_type = {} ) const
    {
        if( everything )
        {
            return true;
        }
        for( u32 i = 0; i < stores.size(); ++i )
        {
            if( reaches( stores[i], stored[i], read, read_type ) )
            {
                return true;
            }
        }
        return false;
    }

    bool invariant( Local_id local ) const
    {
        return writes[local.v] == 0 && !( is_addressed( addressed, local ) && ( everything || anywhere ) );
    }

    // Whether the loop writes no local `place` names, and, for a global, nothing a store may reach.
    bool fixed( const Place& place ) const
    {
        if( place.is_global() ? everything || anywhere : !invariant( place.local ) )
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
        return true;
    }

    bool invariant( const Place& place, Type_id type = {} ) const
    {
        return fixed( place ) && ( !has_deref_projection( func, place ) || !reached( place, type ) );
    }

    bool invariant( const Operand& operand ) const
    {
        return operand.kind == Operand_kind::Constant || operand.kind == Operand_kind::Debug ||
               ( operand.kind == Operand_kind::Copy && invariant( operand.place, operand.type ) );
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
        case Rvalue_kind::Sqrt:
        case Rvalue_kind::Run_of:
        case Rvalue_kind::Run_data:
        case Rvalue_kind::Run_size:
            break;
        case Rvalue_kind::Call:
            if( !pure_call( value ) )
            {
                return false;
            }
            break;
        default:
            return false;
        }

        bool all = true;
        for_each_operand( value, [&]( const Operand& operand ) { all &= invariant( operand ); } );
        return all;
    }

    // Whether the statement at `index` in `block` runs on the loop's first turn, with nothing but stores
    // to memory before it: so a read through a pointer, or a panic, may run in the preheader.
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
            if( observed( func, purity, statements[s] ) )
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
                    if( observed( func, purity, statement ) )
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

bool traps( const Scope& scope, const Rvalue& value )
{
    const Function& func = scope.func;
    if( value.kind == Rvalue_kind::Address_of )
    {
        return false;
    }
    // A pure call that may panic or not return waits as a division does.
    if( value.kind == Rvalue_kind::Call && !scope.purity->removable( value ) )
    {
        return true;
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

void append( Function& func, Block_id block, Statement statement )
{
    insert_statement( func, block, func.blocks[block].statement_count, std::move( statement ) );
}

// Moves one invariant statement or read to the preheader; whether it found one.
bool hoist_one( Function& func, const Scope& scope, bool pin_named )
{
    const Block_id preheader = scope.loop.preheader;
    // The loop test's: a rotated loop tests in its latch.
    const Block_id latch = scope.loop.latch;
    const Block_id head =
        latch.is_valid() && func.blocks[latch].terminator.kind == Terminator_kind::Branch ? latch : scope.loop.header;
    const auto moved_at = [&]( Span span ) { return pin_named ? func.blocks[head].terminator.span : span; };
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
                if( scope.writes[local.v] == 1 && !is_addressed( scope.addressed, local ) && scope.invariant( value ) &&
                    !( pin_named && func.locals[local].name.is_valid() ) )
                {
                    const bool every_entry = scope.runs_unobserved( b, s );
                    if( ( every_entry || !traps( scope, value ) ) && read_only_after( scope, b, s, local, every_entry ) )
                    {
                        const u32 index             = block.first_statement + s;
                        func.statements[index].span = moved_at( statement->span );
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
                    !scope.invariant( operand->place, operand->type ) || !scope.runs_unobserved( b, s ) )
                {
                    continue;
                }

                // Before add_rvalue, which can move the rvalue `operand` lives in.
                const Operand  from  = *operand;
                const Span     span  = at_end ? block.terminator.span : statement->span;
                const Local_id local = func.locals.push_back( Local { .type = from.type, .span = span } );
                *operand             = copy( Place { .local = local }, from.type );
                Statement read {
                    .span  = moved_at( span ),
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

bool is_local( const Operand& operand, Local_id local )
{
    return operand.kind == Operand_kind::Copy && operand.place.is_whole_local() && operand.place.local == local;
}

// `local`'s one write in the loop, at an index in `block`.
std::optional<u32> assigned( const Function& func, const Scope& scope, Local_id local, Block_id block )
{
    if( scope.writes[local.v] != 1 || is_addressed( scope.addressed, local ) )
    {
        return std::nullopt;
    }
    const Run<const Statement> statements = func.statements_of( block );
    for( u32 s = 0; s < statements.size(); ++s )
    {
        if( statements[s].kind == Statement_kind::Assign && statements[s].place.is_whole_local() &&
            statements[s].place.local == local )
        {
            return s;
        }
    }
    return std::nullopt;
}

const Rvalue& value_at( const Function& func, Block_id block, u32 s )
{
    return func.rvalue( func.statements_of( block )[s] );
}

// A value the preheader can read as the loop does.
bool fixed( const Scope& scope, const Operand& operand )
{
    return operand.kind == Operand_kind::Constant ||
           ( operand.kind == Operand_kind::Copy && operand.place.is_whole_local() && scope.invariant( operand ) );
}

// Through the empty blocks normalising gives each exit.
bool panics( const Function& func, Block_id target )
{
    for( u32 hops = 0;
         hops < 4 && func.blocks[target].statement_count == 0 && func.blocks[target].terminator.kind == Terminator_kind::Goto;
         ++hops )
    {
        target = func.blocks[target].terminator.targets[0];
    }
    return func.blocks[target].terminator.kind == Terminator_kind::Panic;
}

// The latch's `x < n`, at `tested_at`: `n`, or what the latch copied into it before the test.
std::optional<Operand> bound_of( const Function& func, const Scope& scope, const Rvalue& tested, u32 tested_at )
{
    if( fixed( scope, tested.b ) )
    {
        return tested.b;
    }
    const Block_id latch = scope.loop.latch;
    if( tested.b.kind == Operand_kind::Copy && tested.b.place.is_whole_local() )
    {
        const std::optional<u32> at = assigned( func, scope, tested.b.place.local, latch );
        if( at && *at < tested_at && value_at( func, latch, *at ).kind == Rvalue_kind::Use &&
            fixed( scope, value_at( func, latch, *at ).a ) )
        {
            return value_at( func, latch, *at ).a;
        }
    }
    return std::nullopt;
}

// A loop whose index `i` runs from `a` by 1 while the latch's `i < n` holds, and whose only other exit
// is a panic on `i >= len`, `len` invariant, with no call, drop or release anywhere in it: the panic
// moves to the preheader, on `a >= len || n > len`. The turns it skips store only to memory.
bool hoist_range_check( Function& func, const Scope& scope, const Literal_pool& literals, bool pin_named )
{
    const Loop&    loop      = scope.loop;
    const Block_id preheader = loop.preheader;
    const Block_id latch     = loop.latch;
    if( scope.everything || !latch.is_valid() || scope.exiting.size() != 2 ||
        func.blocks[preheader].terminator.kind != Terminator_kind::Goto ||
        std::ranges::any_of( loop.blocks, [&]( Block_id b ) { return !scope.own[b.v]; } ) )
    {
        return false;
    }
    const Terminator& test = func.blocks[latch].terminator;
    if( test.kind != Terminator_kind::Branch || test.targets[0] != loop.header || loop.contains( test.targets[1] ) ||
        test.condition.kind != Operand_kind::Copy || !test.condition.place.is_whole_local() )
    {
        return false;
    }
    const Block_id check = scope.exiting[0] == latch ? scope.exiting[1] : scope.exiting[0];
    if( check == latch )
    {
        return false;
    }
    const Terminator& branch = func.blocks[check].terminator;
    if( branch.kind != Terminator_kind::Branch || branch.condition.kind != Operand_kind::Copy ||
        !branch.condition.place.is_whole_local() )
    {
        return false;
    }
    const bool     fails_on_true = !loop.contains( branch.targets[0] );
    const Block_id stays         = branch.targets[fails_on_true ? 1 : 0];
    if( !panics( func, branch.targets[fails_on_true ? 0 : 1] ) || !loop.contains( stays ) ||
        !scope.found.dominators.dominates( check, latch ) )
    {
        return false;
    }

    // The latch steps `i` by 1, then tests `i < n`; the check, before the latch, reads `i` as the turn began.
    const std::optional<u32> tested_at   = assigned( func, scope, test.condition.place.local, latch );
    const std::optional<u32> compared_at = assigned( func, scope, branch.condition.place.local, check );
    if( !tested_at || !compared_at )
    {
        return false;
    }
    const Rvalue& tested   = value_at( func, latch, *tested_at );
    const Rvalue& compared = value_at( func, check, *compared_at );
    if( tested.kind != Rvalue_kind::Binary || tested.op != Token_kind::Less || compared.kind != Rvalue_kind::Binary )
    {
        return false;
    }
    const std::optional<Operand> n = bound_of( func, scope, tested, *tested_at );
    if( !n )
    {
        return false;
    }

    std::optional<Operand> len;
    for( const Induction& i : inductions_of( func, loop, scope.addressed ) )
    {
        if( i.block != latch || i.index >= *tested_at || !is_local( tested.a, i.local ) ||
            i.step.kind != Operand_kind::Constant || literals.kind( i.step.constant ) != Literal_kind::Integer ||
            literals.integer( i.step.constant ) != 1 )
        {
            continue;
        }
        // As `i op len`, failing on true: `i >= len`.
        const bool flipped = is_local( compared.b, i.local );
        if( !flipped && !is_local( compared.a, i.local ) )
        {
            continue;
        }
        const Token_kind fails = flipped ? Token_kind::Less_equal : Token_kind::Greater_equal;
        const Token_kind holds = flipped ? Token_kind::Greater : Token_kind::Less;
        if( compared.op == ( fails_on_true ? fails : holds ) )
        {
            len = flipped ? compared.a : compared.b;
            break;
        }
    }
    if( !len || !fixed( scope, *len ) )
    {
        return false;
    }

    // In the preheader, the check as written, of `a`, then of `n - 1` as `n > len`; joined as the check reads.
    const Span    span      = branch.span;
    const Span    at        = pin_named ? test.span : span;
    const Type_id boolean   = func.locals[branch.condition.place.local].type;
    const auto    temporary = [&]( Rvalue value )
    {
        const Local_id local = func.locals.push_back( Local { .type = boolean, .span = span } );
        append(
            func, preheader, Statement { .span = at, .place = Place { .local = local }, .value = func.add_rvalue( value ) }
        );
        return copy( Place { .local = local }, boolean );
    };
    const Operand first = temporary( compared );
    const Operand last  = temporary(
        fails_on_true ? binary( Token_kind::Less, *len, *n, boolean ) : binary( Token_kind::Less_equal, *n, *len, boolean )
    );
    const Operand either = temporary( binary( fails_on_true ? Token_kind::Pipe : Token_kind::Amp, first, last, boolean ) );
    remove_statements( func, std::vector<bool>( func.statements.size(), false ) );

    func.blocks[preheader].terminator = Terminator {
        .kind = Terminator_kind::Branch, .span = span, .condition = either, .targets = { branch.targets[0], branch.targets[1] }
    };
    func.blocks[preheader].terminator.targets[fails_on_true ? 1 : 0] = loop.header;
    func.blocks[check].terminator = Terminator { .kind = Terminator_kind::Goto, .span = span, .targets = { stays } };
    return true;
}

// `c` where `operand`, read in `block` before statement `before`, is `i + c` for a constant `c`, or `i`.
std::optional<u64> offset_of(
    const Function&     func,
    const Scope&        scope,
    const Literal_pool& literals,
    const Operand&      operand,
    Local_id            i,
    Block_id            block,
    u32                 before
)
{
    if( is_local( operand, i ) )
    {
        return 0;
    }
    if( operand.kind != Operand_kind::Copy || !operand.place.is_whole_local() )
    {
        return std::nullopt;
    }
    const std::optional<u32> at = assigned( func, scope, operand.place.local, block );
    if( !at || *at >= before )
    {
        return std::nullopt;
    }
    const Rvalue& value = value_at( func, block, *at );
    if( value.kind != Rvalue_kind::Binary || value.op != Token_kind::Plus )
    {
        return std::nullopt;
    }
    const Operand& c = is_local( value.a, i ) ? value.b : value.a;
    if( ( !is_local( value.a, i ) && !is_local( value.b, i ) ) || c.kind != Operand_kind::Constant ||
        literals.kind( c.constant ) != Literal_kind::Integer )
    {
        return std::nullopt;
    }
    return literals.integer( c.constant );
}

// A `u64` loop the range check cannot take exactly: its index `i` runs from `a` by `s` while the latch's
// `i + d < n` (or `<=`) holds, and its bounds checks are `i + c < len`, any number of them, or, under
// `i < n` stepping by 1, `o + k < len` with `k` stepped by a fixed stride. Wrapping
// makes the turn a check would fail on unknowable before the loop, so the preheader proves none can
// fail instead, from `a <= n <= max - s - max( d, c )`, `a + c < len` and the last turn's `i + c < len`;
// the loop runs without them when it holds and as it was, in a copy never versioned again, when not.
bool version_range_checks( Function& func, const Scope& scope, Literal_pool& literals, u32 budget )
{
    const Loop&    loop      = scope.loop;
    const Block_id preheader = loop.preheader;
    const Block_id latch     = loop.latch;
    if( budget == 0 || !latch.is_valid() || func.blocks[loop.header].versioned ||
        func.blocks[preheader].terminator.kind != Terminator_kind::Goto ||
        std::ranges::any_of( loop.blocks, [&]( Block_id b ) { return !scope.own[b.v]; } ) )
    {
        return false;
    }
    u32 size = 0;
    for( const Block_id b : loop.blocks )
    {
        for( const Statement& statement : func.statements_of( b ) )
        {
            size += statement.kind == Statement_kind::Assign || statement.kind == Statement_kind::Drop;
        }
    }
    const Terminator& test = func.blocks[latch].terminator;
    if( size > budget || test.kind != Terminator_kind::Branch || test.targets[0] != loop.header ||
        loop.contains( test.targets[1] ) || test.condition.kind != Operand_kind::Copy ||
        !test.condition.place.is_whole_local() )
    {
        return false;
    }
    const std::optional<u32> tested_at = assigned( func, scope, test.condition.place.local, latch );
    if( !tested_at )
    {
        return false;
    }
    const Rvalue& tested = value_at( func, latch, *tested_at );
    if( tested.kind != Rvalue_kind::Binary || ( tested.op != Token_kind::Less && tested.op != Token_kind::Less_equal ) )
    {
        return false;
    }
    const std::optional<Operand> n = bound_of( func, scope, tested, *tested_at );
    if( !n )
    {
        return false;
    }

    // The index, stepped in the latch before its test reads `i + d`.
    const Type_id                word       = scope.table.integer( 64, false );
    const std::vector<Induction> inductions = inductions_of( func, loop, scope.addressed );
    std::optional<u64>           step;
    std::optional<u64>           d;
    Local_id                     i;
    for( const Induction& induction : inductions )
    {
        if( induction.block != latch || induction.index >= *tested_at || func.locals[induction.local].type != word ||
            induction.step.kind != Operand_kind::Constant || literals.kind( induction.step.constant ) != Literal_kind::Integer )
        {
            continue;
        }
        const std::optional<u64> at = offset_of( func, scope, literals, tested.a, induction.local, latch, *tested_at );
        if( at && ( is_local( tested.a, induction.local ) ||
                    *assigned( func, scope, tested.a.place.local, latch ) > induction.index ) )
        {
            step = literals.integer( induction.step.constant );
            d    = at;
            i    = induction.local;
            break;
        }
    }
    constexpr u64 k_small = u64( 1 ) << 32;
    constexpr u64 k_wide  = u64( 1 ) << 62;
    if( !step || *step == 0 || *step >= k_small || *d >= k_small )
    {
        return false;
    }

    // `o + k`, or `k`: `o` fixed, `k` stepped in the latch by a fixed `s`, so `k0 + s * t` on turn `t`;
    // or `k` is `i * s`, `a * s + s * t`.
    struct Strided
    {
        std::optional<Operand> o;
        Local_id               k;
        Operand                s;
        bool                   scaled;
    };
    // `operand`'s `x op y`, written in `b` before statement `before`, and where.
    const auto made = [&]( const Operand& operand, Block_id b, u32 before, Token_kind op ) -> std::optional<u32>
    {
        if( operand.kind != Operand_kind::Copy || !operand.place.is_whole_local() )
        {
            return std::nullopt;
        }
        const std::optional<u32> at = assigned( func, scope, operand.place.local, b );
        if( !at || *at >= before || value_at( func, b, *at ).kind != Rvalue_kind::Binary || value_at( func, b, *at ).op != op )
        {
            return std::nullopt;
        }
        return at;
    };
    const auto term = [&]( const Operand& operand, Block_id b, u32 before ) -> std::optional<Strided>
    {
        for( const Induction& k : inductions )
        {
            if( k.block == latch && is_local( operand, k.local ) && func.locals[k.local].type == word &&
                fixed( scope, k.step ) )
            {
                return Strided { .o = std::nullopt, .k = k.local, .s = k.step, .scaled = false };
            }
        }
        if( const std::optional<u32> at = made( operand, b, before, Token_kind::Star ) )
        {
            const Rvalue& value = value_at( func, b, *at );
            for( const bool flipped : { false, true } )
            {
                const Operand& s = flipped ? value.a : value.b;
                if( is_local( flipped ? value.b : value.a, i ) && fixed( scope, s ) )
                {
                    return Strided { .o = std::nullopt, .k = i, .s = s, .scaled = true };
                }
            }
        }
        return std::nullopt;
    };
    const auto strided = [&]( const Operand& index, Block_id b, u32 before ) -> std::optional<Strided>
    {
        if( std::optional<Strided> alone = term( index, b, before ) )
        {
            return alone;
        }
        const std::optional<u32> at = made( index, b, before, Token_kind::Plus );
        if( !at )
        {
            return std::nullopt;
        }
        const Rvalue& value = value_at( func, b, *at );
        for( const bool flipped : { false, true } )
        {
            std::optional<Strided> by = term( flipped ? value.b : value.a, b, *at );
            const Operand&         o  = flipped ? value.a : value.b;
            if( by && fixed( scope, o ) )
            {
                by->o = o;
                return by;
            }
        }
        return std::nullopt;
    };
    // Turn `t` has `i = a + t` and the last is `n - 1 - a`, as the strided checks need.
    const bool counted = *step == 1 && *d == 0 && tested.op == Token_kind::Less;

    struct Check
    {
        Block_id               block;
        Block_id               stays;
        u64                    c;
        Operand                len;
        std::optional<Strided> strided;
    };
    std::vector<Check> checks;
    for( const Block_id b : scope.exiting )
    {
        const Terminator& branch = func.blocks[b].terminator;
        if( b == latch || branch.kind != Terminator_kind::Branch || branch.condition.kind != Operand_kind::Copy ||
            !branch.condition.place.is_whole_local() )
        {
            continue;
        }
        const bool               fails_on_true = !loop.contains( branch.targets[0] );
        const Block_id           stays         = branch.targets[fails_on_true ? 1 : 0];
        const std::optional<u32> compared_at   = assigned( func, scope, branch.condition.place.local, b );
        if( !loop.contains( stays ) || !panics( func, branch.targets[fails_on_true ? 0 : 1] ) || !compared_at )
        {
            continue;
        }
        // As `i + c op len`, failing on true: `i + c >= len`.
        const Rvalue& compared = value_at( func, b, *compared_at );
        if( compared.kind != Rvalue_kind::Binary )
        {
            continue;
        }
        for( const bool flipped : { false, true } )
        {
            const Operand&   index = flipped ? compared.b : compared.a;
            const Operand&   len   = flipped ? compared.a : compared.b;
            const Token_kind fails = flipped ? Token_kind::Less_equal : Token_kind::Greater_equal;
            const Token_kind holds = flipped ? Token_kind::Greater : Token_kind::Less;
            if( !fixed( scope, len ) || compared.op != ( fails_on_true ? fails : holds ) )
            {
                continue;
            }
            const std::optional<u64> c = offset_of( func, scope, literals, index, i, b, *compared_at );
            if( c && *c < k_small )
            {
                checks.push_back( Check { .block = b, .stays = stays, .c = *c, .len = len, .strided = std::nullopt } );
                break;
            }
            if( const std::optional<Strided> by = counted ? strided( index, b, *compared_at ) : std::nullopt )
            {
                checks.push_back( Check { .block = b, .stays = stays, .c = 0, .len = len, .strided = by } );
                break;
            }
        }
    }
    if( checks.empty() )
    {
        return false;
    }

    // In the preheader, `i` holds `a`. A later turn has `i + d < n`, so `i + c < len` when
    // `n + c + [<=] <= len + d`, written without wrapping either side past the other.
    const Span    span      = test.span;
    const Type_id boolean   = func.locals[test.condition.place.local].type;
    const auto    temporary = [&]( Type_id type, Rvalue value )
    {
        const Local_id local = func.locals.push_back( Local { .type = type, .span = span } );
        append(
            func,
            preheader,
            Statement { .span = span, .place = Place { .local = local }, .value = func.add_rvalue( std::move( value ) ) }
        );
        return copy( Place { .local = local }, type );
    };
    const auto number = [&]( u64 value ) { return constant( literals.add_integer( value ), word ); };
    const auto both   = [&]( const Operand& x, const Operand& y )
    { return temporary( boolean, binary( Token_kind::Amp, x, y, boolean ) ); };
    const Operand a      = copy( Place { .local = i }, word );
    u64           widest = *d;
    for( const Check& check : checks )
    {
        widest = std::max( widest, check.c );
    }
    Operand holds = both(
        temporary( boolean, binary( Token_kind::Less_equal, a, *n, boolean ) ),
        temporary( boolean, binary( Token_kind::Less_equal, *n, number( ~u64( 0 ) - *step - widest ), boolean ) )
    );
    const u64              inclusive = tested.op == Token_kind::Less_equal;
    std::optional<Operand> last_turn;
    for( const Check& check : checks )
    {
        if( check.strided )
        {
            // `a < n`, and `o`, `k0` and `s * (n - 1 - a)` each under 2^62 (each product from both sides
            // under 2^32), so `o + k0 + s * t` never wraps and its last is under `len`. Each bound alone,
            // so constant sizes fold it.
            const auto less = [&]( const Operand& x, const Operand& y )
            { return temporary( boolean, binary( Token_kind::Less, x, y, boolean ) ); };
            if( !last_turn )
            {
                holds     = both( holds, less( a, *n ) );
                last_turn = temporary(
                    word,
                    binary( Token_kind::Minus, temporary( word, binary( Token_kind::Minus, *n, a, word ) ), number( 1 ), word )
                );
                holds = both( holds, less( *last_turn, number( k_small ) ) );
            }
            const Strided& by = *check.strided;
            holds             = both( holds, less( by.s, number( k_small ) ) );
            if( by.scaled )
            {
                holds = both( holds, less( a, number( k_small ) ) );
            }
            const Operand k0      = by.scaled ? temporary( word, binary( Token_kind::Star, a, by.s, word ) )
                                              : copy( Place { .local = by.k }, word );
            const Operand span_of = temporary( word, binary( Token_kind::Star, by.s, *last_turn, word ) );
            holds                 = both( holds, less( k0, number( k_wide ) ) );
            holds                 = both( holds, less( span_of, number( k_wide ) ) );
            Operand last          = temporary( word, binary( Token_kind::Plus, k0, span_of, word ) );
            if( by.o )
            {
                holds = both( holds, less( *by.o, number( k_wide ) ) );
                last  = temporary( word, binary( Token_kind::Plus, *by.o, last, word ) );
            }
            holds = both( holds, less( last, check.len ) );
            continue;
        }
        const Operand first = check.c == 0 ? a : temporary( word, binary( Token_kind::Plus, a, number( check.c ), word ) );
        holds               = both( holds, temporary( boolean, binary( Token_kind::Less, first, check.len, boolean ) ) );
        const u64     more  = check.c + inclusive;
        const Operand left  = more > *d ? temporary( word, binary( Token_kind::Plus, *n, number( more - *d ), word ) ) : *n;
        const Operand right =
            more < *d ? temporary( word, binary( Token_kind::Plus, check.len, number( *d - more ), word ) ) : check.len;
        holds = both( holds, temporary( boolean, binary( Token_kind::Less_equal, left, right, boolean ) ) );
    }
    remove_statements( func, std::vector<bool>( func.statements.size(), false ) );

    std::vector<Block_id> copies;
    clone_blocks( func, loop, copies );
    func.blocks[copies[loop.header.v]].versioned = true;
    func.blocks[preheader].terminator            = Terminator {
                   .kind = Terminator_kind::Branch, .span = span, .condition = holds, .targets = { loop.header, copies[loop.header.v] }
    };
    for( const Check& check : checks )
    {
        func.blocks[check.block].terminator = Terminator {
            .kind = Terminator_kind::Goto, .span = func.blocks[check.block].terminator.span, .targets = { check.stays }
        };
    }
    return true;
}

std::vector<u32> innermost_of( const Function& func, const Loops& found )
{
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
    return innermost;
}
} // namespace

std::optional<std::vector<Blocked_read>>
blocked_reads( const Function& func, const Type_table& table, const Loops& found, u32 index )
{
    const Local_set addressed = addressed_locals_of( func );
    const Scope     scope( func, table, found, index, innermost_of( func, found ), addressed, nullptr );
    if( scope.everything || scope.stores.empty() ||
        std::ranges::any_of( scope.loop.blocks, [&]( Block_id b ) { return !scope.own[b.v]; } ) )
    {
        return std::nullopt;
    }

    std::vector<Blocked_read> reads;
    const auto                add = [&]( const Operand& operand, Block_id b, u32 s )
    {
        if( operand.kind != Operand_kind::Copy || !has_deref_projection( func, operand.place ) ||
            !scope.fixed( operand.place ) )
        {
            return;
        }
        const bool early = scope.runs_unobserved( b, s );
        for( Blocked_read& read : reads )
        {
            if( same_place( func, read.place, operand.place ) )
            {
                read.early |= early;
                return;
            }
        }
        reads.push_back( Blocked_read {
            .place   = operand.place,
            .type    = operand.type,
            .early   = early,
            .reached = scope.reached( operand.place, operand.type )
        } );
    };
    for( const Block_id b : scope.loop.blocks )
    {
        const Run<const Statement> statements = func.statements_of( b );
        for( u32 s = 0; s < statements.size(); ++s )
        {
            if( statements[s].kind == Statement_kind::Assign && func.rvalue( statements[s] ).kind != Rvalue_kind::Address_of )
            {
                for_each_operand( func.rvalue( statements[s] ), [&]( const Operand& operand ) { add( operand, b, s ); } );
            }
        }
        const Terminator& terminator = func.blocks[b].terminator;
        for( const Operand* operand : { &terminator.condition, &terminator.message, &terminator.at } )
        {
            add( *operand, b, statements.size() );
        }
    }
    return reads;
}

bool hoist_invariants(
    Function& func, const Type_table& table, Literal_pool& literals, const Purity& purity, bool pin_named, u32 copying
)
{
    bool changed = false;
    for( bool again = true; again; )
    {
        // The round before merged the preheaders away, and a range check moves one's end.
        changed |= normalise_loops( func );
        again                            = false;
        const Loops            found     = find_loops( func );
        const Local_set        addressed = addressed_locals_of( func );
        const std::vector<u32> innermost = innermost_of( func, found );
        for( u32 l = 0; l < found.loops.size() && !again; ++l )
        {
            if( !found.loops[l].preheader.is_valid() )
            {
                continue;
            }
            while( hoist_one( func, Scope( func, table, found, l, innermost, addressed, &purity ), pin_named ) )
            {
                changed = true;
            }
            // After the reads, so its bound is a local.
            again =
                hoist_range_check( func, Scope( func, table, found, l, innermost, addressed, &purity ), literals, pin_named ) ||
                version_range_checks( func, Scope( func, table, found, l, innermost, addressed, &purity ), literals, copying );
            changed |= again;
        }
    }
    return changed;
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/print.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

constexpr std::string_view k_types = "struct Pair { i32 a; i32 b; };\n"
                                     "struct Buf { i64[*] data; u64 count; i64 last; };\n"
                                     "struct Reals { f64[*] data; u64 count; };\n"
                                     "i32 g = 0;\n"
                                     "void touch() { g += 1; }\n"
                                     "extern void ticked();\n"
                                     "void tick() { unsafe { ticked(); } }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Hoisted : Compiled
{
    // Without versioning, which copies a loop LICM leaves.
    explicit Hoisted(
        std::string_view source, u32 level = 3, std::optional<Loop_weights> loops = Loop_weights { .versioning = 0 }
    )
        : Compiled(
              std::string( source ) + std::string( k_types ), Stage::Elaborated, Build { .level = level, .loops = loops }
          )
    {
        INFO( rendered() );
        REQUIRE( clean() );
        REQUIRE( verify( functions.front() ).empty() );
    }

    std::string kir() const
    {
        return print( functions.front(), ast, types.table(), literals, interner );
    }

    // What `fn` counts in each statement in a loop.
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
                if( inside[b.v] && statement.kind == Statement_kind::Assign )
                {
                    count += fn( func, func.rvalue( statement ) );
                }
            }
        }
        return count;
    }

    // Operands in a loop read through a pointer.
    std::size_t reads_through() const
    {
        return in_loops(
            []( const Function& func, const Rvalue& value )
            {
                std::size_t through = 0;
                for_each_operand(
                    value,
                    [&]( const Operand& operand )
                    { through += operand.kind == Operand_kind::Copy && has_deref_projection( func, operand.place ); }
                );
                return through;
            }
        );
    }

    // Blocks in a loop with an edge out of it.
    std::size_t exits() const
    {
        const Function& func  = functions.front();
        std::size_t     count = 0;
        for( const Loop& loop : find_loops( func ).loops )
        {
            std::vector<Block_id> targets;
            for( const Block_id b : loop.blocks )
            {
                targets.clear();
                successors( func.blocks[b].terminator, targets );
                count += std::ranges::any_of( targets, [&]( Block_id t ) { return !loop.contains( t ); } );
            }
        }
        return count;
    }

    std::size_t calls() const
    {
        return in_loops( []( const Function&, const Rvalue& value ) { return value.kind == Rvalue_kind::Call; } );
    }

    std::size_t by( Token_kind op ) const
    {
        return in_loops(
            [&]( const Function&, const Rvalue& value )
            { return value.kind == Rvalue_kind::Binary && value.op == op && value.b.kind == Operand_kind::Constant; }
        );
    }
};

// A pure function too large to inline: `x` stirred, then `last` added.
std::string stirred( std::string_view last )
{
    std::string source = "i64 mix( i64 a, i64 b ) { i64 x = a; ";
    for( int i = 0; i < 16; ++i )
    {
        source += "x = x * 31 + b; x = x ^ ( x >> 7 ); ";
    }
    return source + "return x + " + std::string( last ) + "; }\n";
}

} // namespace

TEST_CASE( "hoist_invariants_hoists_a_read_through_a_reference_and_a_product", "[opt][hoist_invariants]" )
{
    constexpr std::string_view source = "i64 f( const ref Pair p, i64 n, i64 k ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) "
                                        "{ t += i * p.a * ( k * 4 ); } return t; }\n";
    const Hoisted              f( source );
    REQUIRE( f.reads_through() == 0 );
    REQUIRE( f.by( Token_kind::Star ) == 0 );

    const Hoisted off( source, 3, std::nullopt );
    REQUIRE( off.reads_through() == 1 );
    REQUIRE( off.by( Token_kind::Star ) == 1 );
}

// `touch` writes a global, which `p` may reach, so the next turn's read sees what it wrote.
TEST_CASE( "hoist_invariants_keeps_a_read_a_call_follows", "[opt][hoist_invariants]" )
{
    const Hoisted f( "i64 f( const ref Pair p, i64 n ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { t += p.a; touch(); } "
                     "return t; }\n" );
    REQUIRE( f.reads_through() == 1 );
}

// `c` is never written, so after its first test the loop's test is one part, and rotates: `p.a` is read
// once, on the path where both parts let the body run.
TEST_CASE( "hoist_invariants_keeps_a_read_an_unrotated_loop_may_skip", "[opt][hoist_invariants]" )
{
    const Hoisted f( "i64 f( const ref Pair p, i64 n, bool c ) { i64 t = 0; for( i64 i = 0; i < n && c; i += 1 ) { t += p.a; } "
                     "return t; }\n" );
    REQUIRE( f.reads_through() == 0 );
}

// A test of two parts that both change is not rotated, so the body may not run, and `p.a` stays in it.
TEST_CASE( "hoist_invariants_keeps_a_read_a_two_part_test_may_skip", "[opt][hoist_invariants]" )
{
    const Hoisted f(
        "i64 f( const ref Pair p, i64 n, i64 m ) { i64 t = 0; for( i64 i = 0; i < n && i != m; i += 1 ) { t += p.a; } "
        "return t; }\n"
    );
    REQUIRE( f.reads_through() == 1 );
}

// A division may panic, so it waits for the call before it; without the call it is hoisted. `tick`,
// unlike `touch`, is not inlined into a store.
TEST_CASE( "hoist_invariants_keeps_a_division_a_call_runs_before", "[opt][hoist_invariants]" )
{
    const Hoisted kept( "i64 f( i64 n, i64 k ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { tick(); t += k / 3; } "
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

// A store to `p.b` cannot reach `p.a`, a field beside it through the same pointer, so `p.a` is read
// once; `p.b` is read each turn.
TEST_CASE( "hoist_invariants_hoists_a_field_a_store_to_another_field_cannot_reach", "[opt][hoist_invariants]" )
{
    constexpr std::string_view source = "i32 f( ref Pair p, i32 n ) { i32 t = 0; for( i32 i = 0; i < n; i += 1 ) "
                                        "{ t += p.a + p.b; p.b = t; } return t; }\n";
    const Hoisted              f( source );
    INFO( f.kir() );
    REQUIRE( f.reads_through() == 1 );
    REQUIRE( Hoisted( source, 3, std::nullopt ).reads_through() == 2 );
}

// `d` is new memory, so no store through it reaches `b`: `b.data` and `b.count`, which the test reads
// after the store, are read once.
TEST_CASE( "hoist_invariants_hoists_a_header_past_a_store_to_fresh_memory", "[opt][hoist_invariants]" )
{
    constexpr std::string_view source =
        "void f( const ref Buf b ) { i64[*] d; unsafe { d = alloc<i64>( b.count ); } "
        "for( u64 i = 0; i < b.count; i += 1 ) { unsafe { d[ i ] = b.data[ i ]; } } unsafe { free( d ); } }\n";
    const Hoisted f( source );
    INFO( f.kir() );
    REQUIRE( f.reads_through() == 1 );
    REQUIRE( Hoisted( source, 3, std::nullopt ).reads_through() == 3 );
}

// `ys.data` may point at `xs.count`, a `u64` the `i64` store may change, so it is read each turn; the
// pointers are not (D89).
TEST_CASE( "hoist_invariants_keeps_a_header_a_store_through_a_reference_may_reach", "[opt][hoist_invariants]" )
{
    const Hoisted f( "void f( ref Buf ys, const ref Buf xs ) { for( u64 i = 0; i < xs.count; i += 1 ) "
                     "{ unsafe { ys.data[ i ] = xs.data[ i ]; } } }\n" );
    INFO( f.kir() );
    REQUIRE( f.reads_through() == 2 );
}

// The store writes an `f64`, which cannot change a pointer or a `u64` (D89): both headers are read once,
// and only `xs.data[ i ]` each turn.
TEST_CASE( "hoist_invariants_hoists_a_header_past_a_store_of_another_kind", "[opt][hoist_invariants]" )
{
    constexpr std::string_view source = "void f( ref Reals ys, const ref Reals xs ) { for( u64 i = 0; i < xs.count; i += 1 ) "
                                        "{ unsafe { ys.data[ i ] = xs.data[ i ]; } } }\n";
    const Hoisted              f( source );
    INFO( f.kir() );
    REQUIRE( f.reads_through() == 1 );
    REQUIRE( Hoisted( source, 3, std::nullopt ).reads_through() == 4 );
}

// `i < len` is not the loop's test, and the body only stores, so one check before the loop takes its
// place: the latch is the loop's one exit.
TEST_CASE( "hoist_invariants_hoists_a_range_check_the_loop_test_does_not_cover", "[opt][hoist_invariants]" )
{
    constexpr std::string_view source = "void f( i64[*] d, u64 len, u64 n ) { for( u64 i = 0; i < n; i += 1 ) "
                                        "{ if( i >= len ) { panic( \"short\" ); } unsafe { d[ i ] = 1; } } }\n";
    const Hoisted              f( source );
    INFO( f.kir() );
    REQUIRE( f.exits() == 1 );
    REQUIRE( Hoisted( source, 3, std::nullopt ).exits() == 2 );
}

// Two checks a turn and a step of 2: the preheader chooses the loop without them, one exit, or the
// copy with them, three.
TEST_CASE( "hoist_invariants_versions_a_loop_with_several_range_checks", "[opt][hoist_invariants]" )
{
    constexpr std::string_view source = "void f( i64[*] d, u64 len, u64 n ) { for( u64 i = 0; i + 2 <= n; i += 2 ) "
                                        "{ if( i >= len ) { panic( \"short\" ); } if( i + 1 >= len ) { panic( \"short\" ); } "
                                        "unsafe { d[ i ] = 1; d[ i + 1 ] = 1; } } }\n";
    const Hoisted              f( source, 2, Loop_weights {} );
    INFO( f.kir() );
    REQUIRE( f.exits() == 1 + 3 );
    REQUIRE( Hoisted( source, 2 ).exits() == 3 );
    REQUIRE( Hoisted( source, 1, Loop_weights {} ).exits() == 3 );
}

// An offset index and one stepping by a run-time stride, as a broadcast reads them.
TEST_CASE( "hoist_invariants_versions_a_loop_with_strided_range_checks", "[opt][hoist_invariants]" )
{
    constexpr std::string_view source = "void f( ref f64[] d, const ref f64[] a, u64 o, u64 s, u64 n ) "
                                        "{ for( u64 j : 0..n ) { d[ o + j ] = a[ j * s ]; } }\n";
    const Hoisted              f( source, 2, Loop_weights {} );
    INFO( f.kir() );
    REQUIRE( f.exits() == 1 + 3 );
    REQUIRE( Hoisted( source, 2 ).exits() == 3 );
}

// `tick` runs before the check on each turn, and a panic before the loop would skip it.
TEST_CASE( "hoist_invariants_keeps_a_range_check_a_call_comes_before", "[opt][hoist_invariants]" )
{
    const Hoisted f( "void f( i64[*] d, u64 len, u64 n ) { for( u64 i = 0; i < n; i += 1 ) "
                     "{ tick(); if( i >= len ) { panic( \"short\" ); } unsafe { d[ i ] = 1; } } }\n" );
    INFO( f.kir() );
    REQUIRE( f.exits() == 2 );
}

// `mix` is pure and surely returns, so it moves as a product would; `tick` calls an extern.
TEST_CASE( "hoist_invariants_hoists_a_pure_call", "[opt][hoist_invariants]" )
{
    const Hoisted f(
        "i64 f( i64 n, i64 a, i64 b ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) "
        "{ t += i * mix( a, b ); } return t; }\n" +
        stirred( "0" )
    );
    INFO( f.kir() );
    REQUIRE( f.calls() == 0 );

    const Hoisted g( "i64 f( i64 n ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { tick(); t += i; } return t; }\n" );
    INFO( g.kir() );
    REQUIRE( g.calls() == 1 );
}

// A pure call is no store, so a read through a reference after it still moves.
TEST_CASE( "hoist_invariants_hoists_a_read_past_a_pure_call", "[opt][hoist_invariants]" )
{
    const Hoisted f(
        "i64 f( const ref Pair p, i64 n, i64 a ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) "
        "{ t += mix( a, i ) + p.a; } return t; }\n" +
        stirred( "0" )
    );
    INFO( f.kir() );
    REQUIRE( f.calls() == 1 );
    REQUIRE( f.reads_through() == 0 );
}

// `mix` divides, so it may panic: it moves only from where it runs on every turn, as a division does.
TEST_CASE( "hoist_invariants_keeps_a_pure_call_that_may_panic_under_a_branch", "[opt][hoist_invariants]" )
{
    const Hoisted f(
        "i64 f( i64 n, i64 a, i64 b ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) "
        "{ t += i * mix( a, b ); } return t; }\n" +
        stirred( "a / b" )
    );
    INFO( f.kir() );
    REQUIRE( f.calls() == 0 );

    const Hoisted g(
        "i64 f( i64 n, i64 a, i64 b ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) "
        "{ if( i > 2 ) { t += mix( a, b ); } } return t; }\n" +
        stirred( "a / b" )
    );
    INFO( g.kir() );
    REQUIRE( g.calls() == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
