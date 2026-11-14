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
// reads memory after one.
bool observed( const Function& func, const Statement& statement )
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
    return kind == Rvalue_kind::Call || kind == Rvalue_kind::Indirect_call || kind == Rvalue_kind::Release;
}

// One loop, as the function now stands.
struct Scope
{
    const Function&                         func;
    const Loops&                            found;
    const Loop&                             loop;
    const Local_set&                        addressed;
    std::vector<bool>                       own;     // by block: in the loop and in none inside it
    std::vector<u32>                        writes;  // by local: statements in the loop that write it or take its address
    std::vector<Block_id>                   exiting; // blocks in the loop with an edge out
    std::vector<u32>                        sets;    // by local: the same over the whole function
    std::vector<std::vector<const Rvalue*>> sources; // by local: the rvalues assigned to all of it
    std::vector<Place>                      stores;  // stores in the loop through a pointer
    bool everything = false;                         // a call, drop, release, or store to a global or an addressed local
    bool anywhere   = false;                         // a store through a pointer that may reach memory from before it

    Scope( const Function& f, const Loops& l, u32 index, const std::vector<u32>& innermost, const Local_set& a )
        : func( f ),
          found( l ),
          loop( l.loops[index] ),
          addressed( a ),
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
                if( !clobbers( func, addressed, statement ) )
                {
                    continue;
                }
                const Rvalue_kind kind =
                    statement.kind == Statement_kind::Assign ? func.rvalue( statement ).kind : Rvalue_kind::Call;
                if( kind == Rvalue_kind::Call || kind == Rvalue_kind::Indirect_call || kind == Rvalue_kind::Release ||
                    !has_deref_projection( func, statement.place ) )
                {
                    everything = true;
                }
                else
                {
                    stores.push_back( statement.place );
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

    // Whether a store to `store`, through a pointer, may change `read`, read through one.
    bool reaches( const Place& store, const Place& read ) const
    {
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

    bool reached( const Place& read ) const
    {
        return everything || std::ranges::any_of( stores, [&]( const Place& store ) { return reaches( store, read ); } );
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

    bool invariant( const Place& place ) const
    {
        return fixed( place ) && ( !has_deref_projection( func, place ) || !reached( place ) );
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
        case Rvalue_kind::Sqrt:
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
            if( observed( func, statements[s] ) )
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
                    if( observed( func, statement ) )
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
                    if( ( every_entry || !traps( func, value ) ) && read_only_after( scope, b, s, local, every_entry ) )
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
    // Through the empty blocks normalising gives each exit.
    Block_id panic = branch.targets[fails_on_true ? 0 : 1];
    for( u32 hops = 0;
         hops < 4 && func.blocks[panic].statement_count == 0 && func.blocks[panic].terminator.kind == Terminator_kind::Goto;
         ++hops )
    {
        panic = func.blocks[panic].terminator.targets[0];
    }
    if( func.blocks[panic].terminator.kind != Terminator_kind::Panic || !loop.contains( stays ) ||
        !scope.found.dominators.dominates( check, latch ) )
    {
        return false;
    }

    // `local`'s one write in the loop, at an index in `block`.
    const auto assigned = [&]( Local_id local, Block_id block ) -> std::optional<u32>
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
    };
    const auto value_at = [&]( Block_id block, u32 s ) -> const Rvalue&
    { return func.rvalue( func.statements_of( block )[s] ); };
    // A value the preheader can read as the loop does.
    const auto fixed = [&]( const Operand& operand )
    {
        return operand.kind == Operand_kind::Constant ||
               ( operand.kind == Operand_kind::Copy && operand.place.is_whole_local() && scope.invariant( operand ) );
    };

    // The latch steps `i` by 1, then tests `i < n`; the check, before the latch, reads `i` as the turn began.
    const std::optional<u32> tested_at   = assigned( test.condition.place.local, latch );
    const std::optional<u32> compared_at = assigned( branch.condition.place.local, check );
    if( !tested_at || !compared_at )
    {
        return false;
    }
    const Rvalue& tested   = value_at( latch, *tested_at );
    const Rvalue& compared = value_at( check, *compared_at );
    if( tested.kind != Rvalue_kind::Binary || tested.op != Token_kind::Less || compared.kind != Rvalue_kind::Binary )
    {
        return false;
    }
    // `n`, or what the latch copied into it before the test.
    std::optional<Operand> n;
    if( fixed( tested.b ) )
    {
        n = tested.b;
    }
    else if( tested.b.kind == Operand_kind::Copy && tested.b.place.is_whole_local() )
    {
        const std::optional<u32> at = assigned( tested.b.place.local, latch );
        if( at && *at < *tested_at && value_at( latch, *at ).kind == Rvalue_kind::Use && fixed( value_at( latch, *at ).a ) )
        {
            n = value_at( latch, *at ).a;
        }
    }
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
    if( !len || !fixed( *len ) )
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

std::optional<std::vector<Blocked_read>> blocked_reads( const Function& func, const Loops& found, u32 index )
{
    const Local_set addressed = addressed_locals_of( func );
    const Scope     scope( func, found, index, innermost_of( func, found ), addressed );
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
            .place = operand.place, .type = operand.type, .early = early, .reached = scope.reached( operand.place )
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

bool hoist_invariants( Function& func, const Literal_pool& literals, bool pin_named )
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
            while( hoist_one( func, Scope( func, found, l, innermost, addressed ), pin_named ) )
            {
                changed = true;
            }
            // After the reads, so its bound is a local.
            again = hoist_range_check( func, Scope( func, found, l, innermost, addressed ), literals, pin_named );
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

// A test of two parts is not rotated, so the body may not run, and `p.a` stays in it.
TEST_CASE( "hoist_invariants_keeps_a_read_an_unrotated_loop_may_skip", "[opt][hoist_invariants]" )
{
    const Hoisted f( "i64 f( const ref Pair p, i64 n, bool c ) { i64 t = 0; for( i64 i = 0; i < n && c; i += 1 ) { t += p.a; } "
                     "return t; }\n" );
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

// `ys.data` may point at `xs` or at `ys` itself, so both headers are read each turn.
TEST_CASE( "hoist_invariants_keeps_a_header_a_store_through_a_reference_may_reach", "[opt][hoist_invariants]" )
{
    const Hoisted f( "void f( ref Buf ys, const ref Buf xs ) { for( u64 i = 0; i < xs.count; i += 1 ) "
                     "{ unsafe { ys.data[ i ] = xs.data[ i ]; } } }\n" );
    INFO( f.kir() );
    REQUIRE( f.reads_through() == 4 );
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

// `tick` runs before the check on each turn, and a panic before the loop would skip it.
TEST_CASE( "hoist_invariants_keeps_a_range_check_a_call_comes_before", "[opt][hoist_invariants]" )
{
    const Hoisted f( "void f( i64[*] d, u64 len, u64 n ) { for( u64 i = 0; i < n; i += 1 ) "
                     "{ tick(); if( i >= len ) { panic( \"short\" ); } unsafe { d[ i ] = 1; } } }\n" );
    INFO( f.kir() );
    REQUIRE( f.exits() == 2 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
