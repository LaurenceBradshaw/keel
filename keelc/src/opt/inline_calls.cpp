// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/inline_calls.h"
#include <limits>
#include "ir/call_graph.h"
#include "ir/loops.h"
#include "ir/simplify.h"
#include "opt/propagate_constants.h"
#include "opt/propagate_values.h"
#include "opt/recognise_idioms.h"
#include "opt/remove_dead_stores.h"

namespace keel
{
namespace
{

// What a pick chose: no callee leaves the statement; a receiver stands in for the callee's `this`.
struct Site
{
    const Function*      callee = nullptr;
    std::optional<Place> receiver;
};

// Calls and drops dominate; storage markers, gotos and returns are free.
u32 inline_cost( const Function& callee, const Type_table& types, const Inline_weights& weights )
{
    u32 cost      = 0;
    u32 call_like = 0;

    for( const Statement& statement : callee.statements )
    {
        switch( statement.kind )
        {
        case Statement_kind::Assign:
        {
            const Rvalue&     value = callee.rvalue( statement );
            const Rvalue_kind kind  = value.kind;
            if( kind == Rvalue_kind::Run_data ||
                ( kind == Rvalue_kind::Binary && value.op == Token_kind::Plus && value.b.kind == Operand_kind::Constant &&
                  ( types.is_pointer( value.type ) || types.is_many_pointer( value.type ) ) ) )
            {
                cost += weights.address;
            }
            else if( kind == Rvalue_kind::Call || kind == Rvalue_kind::Indirect_call || kind == Rvalue_kind::Allocate ||
                     kind == Rvalue_kind::Release )
            {
                cost += weights.call;
                call_like++;
            }
            // A dynamic call's own operands, which lower_dispatch spelt out: the call already counts them.
            else if( kind != Rvalue_kind::Vtable_slot && kind != Rvalue_kind::Interface_self )
            {
                cost += weights.statement;
            }
            break;
        }
        case Statement_kind::Drop:
            cost += weights.call;
            call_like++;
            break;
        case Statement_kind::Storage_live:
        case Statement_kind::Storage_dead:
            break;
        }
    }

    for( const Block& block : callee.blocks )
    {
        if( block.terminator.kind == Terminator_kind::Branch )
        {
            cost += weights.branch;
        }
        else if( block.terminator.kind == Terminator_kind::Panic )
        {
            cost += weights.panic;
        }
    }

    // Forwarder bonus: a function that only calls another is cheaper to inline than one that does the same and more.
    if( call_like == 1 )
    {
        cost -= weights.call;
    }

    return cost;
}

// Measured against the caller's size before any splice, so one caller's limit cannot move mid-way.
u32 threshold( const u32 statements_at_start, const Function& callee, const Inline_weights& weights, bool in_loop )
{
    if( statements_at_start >= weights.big_caller )
    {
        return weights.big_caller_threshold;
    }

    const u32 loop = in_loop ? weights.in_loop : 0;
    if( narrow_cast<u32>( callee.blocks.size() ) <= weights.few_blocks )
    {
        return weights.threshold + weights.threshold / 4 + loop;
    }

    return weights.threshold + loop;
}

// An `out` parameter is reached through `&init`, whose pointer `addressed_locals` trusts to die with its call.
bool inlinable( const Function& callee )
{
    if( callee.out_parameters.size() > 0 )
    {
        return false;
    }

    return true;
}

// The offset in `block`, before `index`, of the `&init` that last wrote `pointer`'s local; none if
// another write comes between, or the `&init` is in an earlier block.
std::optional<u32> initialised( const Function& caller, const Block& block, u32 index, const Operand& pointer )
{
    const std::optional<u32> s = last_write( caller, block, index, pointer );
    if( !s )
    {
        return std::nullopt;
    }

    const Rvalue& value = caller.rvalue( caller.statements_of( block )[*s] );
    if( value.kind == Rvalue_kind::Address_of && value.address_purpose == Address_purpose::Initialise )
    {
        return s;
    }

    return std::nullopt;
}

// The constant count of the `run( p, const n )` that `operand` holds at `index` of `at`, through whole
// copies, reborrows and back from a lone predecessor's goto, so through a spliced `as_run`; none past
// a few hops.
std::optional<Operand> run_count( const Function& caller, Block_id at, u32 index, Operand operand )
{
    for( u32 hops = 0; hops < 8; ++hops )
    {
        operand.kind               = operand.kind == Operand_kind::Move ? Operand_kind::Copy : operand.kind;
        const std::optional<u32> s = last_write( caller, caller.blocks[at], index, operand );
        if( !s )
        {
            std::optional<Block_id> from;
            u32                     entries = 0;
            for( const Block_id b : caller.blocks.ids() )
            {
                const Terminator& end = caller.blocks[b].terminator;
                if( end.kind == Terminator_kind::Goto && end.targets[0] == at )
                {
                    from = b;
                    ++entries;
                }
                else if( end.kind == Terminator_kind::Branch && ( end.targets[0] == at || end.targets[1] == at ) )
                {
                    ++entries;
                }
            }
            if( entries != 1 || !from || at == Block_id { 0 } )
            {
                return std::nullopt;
            }
            at    = *from;
            index = caller.blocks[at].statement_count;
            continue;
        }

        const Rvalue& value = caller.rvalue( caller.statements_of( at )[*s] );
        if( value.kind == Rvalue_kind::Run_of && value.b.kind == Operand_kind::Constant )
        {
            return value.b;
        }
        // `&(*p)` is `p`, as collapse_borrows will make it.
        const bool reborrow = value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() &&
                              value.a.place.num_projections == 1 &&
                              caller.projections[value.a.place.first_projection].kind == Projection_kind::Deref;
        if( reborrow )
        {
            operand = copy( Place { .local = value.a.place.local }, value.type );
        }
        else if( value.kind == Rvalue_kind::Use && value.a.has_place() && value.a.place.is_whole_local() )
        {
            operand = value.a;
        }
        else
        {
            return std::nullopt;
        }
        index = *s;
    }

    return std::nullopt;
}

// The whole local `argument` points at, through `&x` written in `at` before `index`; none otherwise.
Local_id borrowed_local( const Function& caller, Block_id at, u32 index, Operand argument )
{
    argument.kind              = argument.kind == Operand_kind::Move ? Operand_kind::Copy : argument.kind;
    const std::optional<u32> s = last_write( caller, caller.blocks[at], index, argument );
    if( !s )
    {
        return {};
    }

    const Rvalue& value = caller.rvalue( caller.statements_of( at )[*s] );
    if( value.kind != Rvalue_kind::Address_of || !value.a.place.is_whole_local() ||
        ( value.address_purpose != Address_purpose::Borrow && value.address_purpose != Address_purpose::Read ) )
    {
        return {};
    }

    return value.a.place.local;
}

// In `copy`, each pointer parameter the call gives a distinct whole number local becomes a local of
// its own, as splicing leaves it: `(*p)` reads and writes that local, loaded at entry and stored at
// each return. Answers how many statements that added, which stand for nothing at the call site.
u32 bind_borrowed_locals(
    Function& copy, const Function& caller, Block_id at, u32 index, const Rvalue& call, const Type_table& types
)
{
    const u32             parameters = std::min( copy.parameter_count, narrow_cast<u32>( call.arguments.size() ) );
    std::vector<Local_id> target( parameters );
    for( u32 i = 0; i < parameters; ++i )
    {
        const Type_id type = copy.locals[Local_id { i + 1 }].type;
        if( types.is_pointer( type ) && types.is_builtin( types.get( type ).element ) )
        {
            target[i] = borrowed_local( caller, at, index, call.arguments[i] );
        }
    }

    // Two pointers at one local alias, and a whole use or a write of the pointer lets it escape or move.
    std::vector<bool> bindable( copy.locals.size(), false );
    for( u32 i = 0; i < parameters; ++i )
    {
        bindable[i + 1] = target[i].is_valid() && std::ranges::count( target, target[i] ) == 1;
    }
    for_each_place(
        std::as_const( copy ),
        [&]( const Place& place, const Statement* )
        {
            if( !place.is_global() && place.local.is_valid() && place.local.v < bindable.size() &&
                ( place.num_projections == 0 || copy.projections[place.first_projection].kind != Projection_kind::Deref ) )
            {
                bindable[place.local.v] = false;
            }
        }
    );

    std::vector<Local_id> fresh( copy.locals.size() );
    for( u32 i = 0; i < parameters; ++i )
    {
        const Local_id parameter { i + 1 };
        if( bindable[parameter.v] )
        {
            const Type_id pointee = types.get( copy.locals[parameter].type ).element;
            fresh[parameter.v]    = copy.locals.push_back( Local { .type = pointee, .span = copy.locals[parameter].span } );
        }
    }

    for_each_place(
        copy,
        [&]( Place& place, const Statement* )
        {
            if( !place.is_global() && place.local.is_valid() && place.local.v < fresh.size() &&
                fresh[place.local.v].is_valid() )
            {
                place.local = fresh[place.local.v];
                place.first_projection++;
                place.num_projections--;
            }
        }
    );

    u32 added = 0;
    for( u32 i = 0; i < parameters; ++i )
    {
        const Local_id parameter { i + 1 };
        if( !fresh[parameter.v].is_valid() )
        {
            continue;
        }

        const Local_id local   = fresh[parameter.v];
        const Type_id  pointee = copy.locals[local].type;
        const Place    through = projected( copy, Place { .local = parameter }, Projection { .kind = Projection_kind::Deref } );
        const Span     span    = copy.locals[parameter].span;

        insert_statement(
            copy,
            Block_id { 0 },
            0,
            Statement {
                .span  = span,
                .place = Place { .local = local },
                .value = copy.add_rvalue( use( keel::copy( through, pointee ) ) )
            }
        );
        ++added;

        for( const Block_id b : copy.blocks.ids() )
        {
            if( copy.blocks[b].terminator.kind == Terminator_kind::Return )
            {
                insert_statement(
                    copy,
                    b,
                    copy.blocks[b].statement_count,
                    Statement {
                        .span  = span,
                        .place = through,
                        .value = copy.add_rvalue( use( keel::copy( Place { .local = local }, pointee ) ) )
                    }
                );
                ++added;
            }
        }
    }

    return added;
}

// `callee` priced as the call at `index` of `at` leaves it: a constant argument assigned to its
// parameter at entry, `size` of a run argument of constant count read as that count, a pointer at a
// distinct local read as that local, then folded. Each assignment stands for the one splicing makes,
// which `inline_cost` never sees, so is not counted. `straight` rotates the loops first, and prices
// only a body none is left in.
u32 folded_cost(
    const Function&       callee,
    const Function&       caller,
    Block_id              at,
    u32                   index,
    const Type_table&     types,
    Literal_pool&         literals,
    const Purity&         purity,
    const Inline_weights& weights,
    bool                  straight = false
)
{
    Function                            copy  = callee;
    const Statement&                    site  = caller.statements_of( at )[index];
    const Rvalue&                       call  = caller.rvalue( site );
    u32                                 bound = 0;
    std::vector<bool>                   written( copy.locals.size(), false );
    std::vector<std::optional<Operand>> counts( copy.locals.size() );

    for( const Statement& statement : copy.statements )
    {
        if( ( statement.kind == Statement_kind::Assign || statement.kind == Statement_kind::Drop ) &&
            !statement.place.is_global() && statement.place.local.is_valid() )
        {
            written[statement.place.local.v] = true;
        }
    }

    for( u32 i = 0; i < copy.parameter_count && i < call.arguments.size(); ++i )
    {
        const Local_id parameter { i + 1 };
        const Operand& argument = call.arguments[i];
        if( argument.kind == Operand_kind::Constant )
        {
            insert_statement(
                copy,
                Block_id { 0 },
                0,
                Statement {
                    .span = site.span, .place = Place { .local = parameter }, .value = copy.add_rvalue( use( argument ) )
                }
            );
            ++bound;
        }
        else if( !written[parameter.v] )
        {
            counts[parameter.v] = run_count( caller, at, index, argument );
        }
    }

    bound += bind_borrowed_locals( copy, caller, at, index, call, types );
    // With nothing from the site, no loop runs once that the finished callee did not fold itself.
    if( straight && bound == 0 && std::ranges::none_of( counts, []( const auto& c ) { return c.has_value(); } ) )
    {
        return std::numeric_limits<u32>::max();
    }

    for( Statement& statement : copy.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }
        const Rvalue& value = std::as_const( copy ).rvalue( statement );
        if( value.kind == Rvalue_kind::Run_size && value.a.kind == Operand_kind::Copy && value.a.place.is_whole_local() &&
            counts[value.a.place.local.v] )
        {
            copy.rvalue( statement ) = use( *counts[value.a.place.local.v] );
        }
    }

    remove_statements( copy, std::vector<bool>( copy.statements.size(), false ) );
    propagate_constants( copy, types, literals, true );
    // Rotated, a loop the call runs once has a constant latch, and goes, as it will once spliced.
    if( straight && rotate_loops( copy, Loop_weights {} ) )
    {
        propagate_constants( copy, types, literals, true );
    }
    propagate_values( copy, types, literals );
    recognise_idioms( copy, types, literals );
    remove_dead_stores( copy, false, purity, types, literals );
    simplify( copy, literals );
    // A loop left would still grow in the caller's loop phase, which this price does not see.
    if( straight && !find_loops( copy ).loops.empty() )
    {
        return std::numeric_limits<u32>::max();
    }
    const u32 cost = inline_cost( copy, types, weights );
    return cost - std::min( cost, bound * weights.statement );
}

// The call at statement `index` of block `at` becomes: `at` up to the call, then the parameters
// assigned from the arguments; the callee's blocks, each `return` a goto; and a continuation that
// moves the result into the call's place, then runs the rest of `at`. `at`'s old range is left
// unreferenced, for the caller to sweep. With a `receiver`, `(*this)` is that place and `this` gets
// no local; a constructor's `&init` moves to the continuation, so the body writes no borrowed local.
void splice( Function& caller, Block_id at, u32 index, const Function& callee, std::optional<Place> receiver )
{
    // Copies: `blocks`, `statements` and `rvalues` grow below.
    const Block              block      = caller.blocks[at];
    const Statement_kind     call_kind  = caller.statements_of( block )[index].kind;
    const Span               call_span  = caller.statements_of( block )[index].span;
    const Place              call_place = caller.statements_of( block )[index].place;
    const Rvalue             call_value = caller.rvalue( caller.statements_of( block )[index] );
    const std::optional<u32> built      = receiver && call_kind == Statement_kind::Assign
                                              ? initialised( caller, block, index, call_value.arguments[0] )
                                              : std::nullopt;

    // A clone: `at`'s old range stays whole, unreferenced, until the caller sweeps it.
    const auto take = [&]( u32 s )
    {
        Statement statement = caller.clone( caller.statements_of( block )[s] );
        caller.statements.push_back( std::move( statement ) );
    };

    const u32      local_base = narrow_cast<u32>( caller.locals.size() );
    const u32      block_base = narrow_cast<u32>( caller.blocks.size() );
    const u32      proj_base  = narrow_cast<u32>( caller.projections.size() );
    const Local_id result { local_base };
    const Block_id continuation { block_base + narrow_cast<u32>( callee.blocks.size() ) };

    for( const Local& local : callee.locals )
    {
        caller.locals.push_back( local );
    }

    for( const Projection& projection : callee.projections )
    {
        Projection copy = projection;
        if( copy.kind == Projection_kind::Index )
        {
            copy.index.v += local_base;
        }
        caller.projections.push_back( copy );
    }

    // A global's local is invalid but its projections still move.
    const auto renumber_place = [&]( Place& place )
    {
        if( receiver && !place.is_global() && place.local == Local_id { 1 } )
        {
            assert( place.num_projections > 0 );
            const u32 rest  = place.num_projections - 1;
            const u32 first = narrow_cast<u32>( caller.projections.size() );
            for( u32 k = 0; k < receiver->num_projections; k++ )
            {
                const Projection projection = caller.projections[receiver->first_projection + k];
                caller.projections.push_back( projection );
            }
            for( u32 k = 0; k < rest; k++ )
            {
                const Projection projection = caller.projections[proj_base + place.first_projection + 1 + k];
                caller.projections.push_back( projection );
            }

            place                  = *receiver;
            place.first_projection = first;
            place.num_projections  = receiver->num_projections + rest;
            return;
        }

        if( place.local.is_valid() )
        {
            place.local.v += local_base;
        }
        if( place.num_projections > 0 )
        {
            place.first_projection += proj_base;
        }
    };

    // By place, not by kind: an `Address_of` operand is a Constant holding one.
    const auto renumber = [&]( Operand& operand ) { renumber_place( operand.place ); };

    for( const Block& from : callee.blocks )
    {
        Block into           = from;
        into.first_statement = narrow_cast<u32>( caller.statements.size() );

        for( const Statement& from_statement : callee.statements_of( from ) )
        {
            Rvalue value = callee.rvalue( from_statement );
            renumber( value.a );
            renumber( value.b );
            for( Operand& argument : value.arguments )
            {
                renumber( argument );
            }

            Statement statement = caller.copy_of( from_statement, std::move( value ) );
            renumber_place( statement.place );
            if( statement.drop_flag.is_valid() )
            {
                statement.drop_flag.v += local_base;
            }
            caller.statements.push_back( std::move( statement ) );
        }

        Terminator& terminator = into.terminator;
        if( terminator.kind == Terminator_kind::Return )
        {
            terminator.kind       = Terminator_kind::Goto;
            terminator.targets[0] = continuation;
        }
        else
        {
            for( Block_id& target : terminator.targets )
            {
                if( target.is_valid() )
                {
                    target.v += block_base;
                }
            }
        }
        renumber( terminator.condition );
        renumber( terminator.message );
        renumber( terminator.at );

        caller.blocks.push_back( into );
    }

    // The continuation: the result into the call's place, then the rest of `at`.
    Block after { .first_statement = narrow_cast<u32>( caller.statements.size() ), .terminator = block.terminator };

    if( built )
    {
        take( *built );
    }

    if( callee.returns_a_value )
    {
        const Place from { .local = result };
        caller.statements.push_back( Statement {
            .kind  = Statement_kind::Assign,
            .span  = call_span,
            .place = call_place,
            .value = caller.add_rvalue( use( move( from, callee.locals[k_return_slot].type ) ) )
        } );
        caller.statements.push_back( Statement { .kind = Statement_kind::Storage_dead, .span = call_span, .place = from } );
    }

    for( u32 s = index + 1; s < block.statement_count; ++s )
    {
        take( s );
    }

    after.statement_count = narrow_cast<u32>( caller.statements.size() ) - after.first_statement;
    caller.blocks.push_back( after );

    // `at`: its statements before the call, then the parameters' storage and values.
    const u32 head_first = narrow_cast<u32>( caller.statements.size() );

    for( u32 s = 0; s < index; ++s )
    {
        if( built && s == *built )
        {
            continue;
        }

        take( s );
    }

    for( u32 p = callee.returns_a_value ? 0 : 1; p <= callee.parameter_count; ++p )
    {
        if( p == 1 && receiver )
        {
            continue;
        }

        caller.statements.push_back( Statement {
            .kind = Statement_kind::Storage_live, .span = call_span, .place = Place { .local = Local_id { local_base + p } }
        } );
    }

    assert( call_kind == Statement_kind::Drop || call_value.arguments.size() == callee.parameter_count );
    for( u32 i = 0; i < call_value.arguments.size(); ++i )
    {
        if( i == 0 && receiver )
        {
            continue;
        }

        caller.statements.push_back( Statement {
            .kind  = Statement_kind::Assign,
            .span  = call_span,
            .place = Place { .local = Local_id { local_base + 1 + i } },
            .value = caller.add_rvalue( use( call_value.arguments[i] ) ),
        } );
    }

    Block& head          = caller.blocks[at];
    head.first_statement = head_first;
    head.statement_count = narrow_cast<u32>( caller.statements.size() ) - head_first;
    head.terminator = Terminator { .kind = Terminator_kind::Goto, .span = call_span, .targets = { Block_id { block_base } } };

    for( const Local_id temporary : callee.one_path_temporaries )
    {
        caller.one_path_temporaries.push_back( Local_id { temporary.v + local_base } );
    }

    // A local's run moves as one, so `parent` and `end` keep their distance from the slot. The
    // receiver's run describes a referent, which the caller's place now is.
    for( u32 i = 0; i < callee.fields.size(); ++i )
    {
        Field_slot slot = callee.fields[i];

        if( slot.local == callee.constructed )
        {
            continue;
        }

        const u32 placed = narrow_cast<u32>( caller.fields.size() );
        slot.local.v += local_base;
        slot.end = placed + ( slot.end - i );
        if( slot.parent != k_no_field )
        {
            slot.parent = placed - ( i - slot.parent );
        }
        caller.fields.push_back( slot );
    }
}

// Whether the receiver is only ever dereferenced, so the place it points at can stand in for it.
bool replaceable_receiver( const Function& callee )
{
    const auto ok = [&]( const Place& place )
    {
        if( !place.local.is_valid() || place.local != Local_id { 1 } ||
            ( place.num_projections > 0 && callee.projections[place.first_projection].kind == Projection_kind::Deref ) )
        {
            return true;
        }
        return false;
    };

    for( const Statement& statement : callee.statements )
    {
        if( !ok( statement.place ) )
        {
            return false;
        }

        bool all_ok = true;
        for_each_operand(
            callee.rvalue( statement ),
            [&]( const Operand& operand )
            {
                if( !ok( operand.place ) )
                {
                    all_ok = false;
                }
            }
        );

        if( !all_ok )
        {
            return false;
        }
    }

    for( const Block& block : callee.blocks )
    {
        if( !ok( block.terminator.condition.place ) || !ok( block.terminator.message.place ) ||
            !ok( block.terminator.at.place ) )
        {
            return false;
        }
    }

    return true;
}

// The `&f` an indirect call's callee always holds: its local assigned once, that or a copy of one
// that does, and never addressed. Spliced-out statements are skipped, so the blocks are walked.
const Rvalue* known_function( const Function& caller, const Operand& callee )
{
    if( !callee.has_place() || !callee.place.is_whole_local() )
    {
        return nullptr;
    }

    Local_id local = callee.place.local;

    for( u32 hops = 0; hops < caller.locals.size(); ++hops )
    {
        const Rvalue* only    = nullptr;
        u32           writes  = 0;
        bool          address = false;

        for( const Block& block : caller.blocks )
        {
            for( const Statement& statement : caller.statements_of( block ) )
            {
                if( statement.kind != Statement_kind::Assign )
                {
                    continue;
                }

                const Rvalue& value = caller.rvalue( statement );
                address             = address || ( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() &&
                                       value.a.place.local == local );
                if( !statement.place.is_global() && statement.place.local == local )
                {
                    ++writes;
                    only = statement.place.is_whole_local() ? &value : nullptr;
                }
            }
        }

        if( writes != 1 || address || only == nullptr )
        {
            return nullptr;
        }

        if( only->kind == Rvalue_kind::Function_address )
        {
            return only;
        }

        if( only->kind != Rvalue_kind::Use || !only->a.has_place() || !only->a.place.is_whole_local() )
        {
            return nullptr;
        }

        local = only->a.place.local;
    }

    return nullptr;
}

// Splices whatever `pick` chooses, then rescans the continuation for more. A spliced body's calls are
// offered again, priced at this site: it may be in a loop, or pass constants, and an indirect one may
// now be known; a call of a callee whose splice it came from is not, nor of one that calls itself, so a
// recursive function is still inlined once. `pick` is told the call's block and which of the caller's
// first blocks it came from.
template <typename Pick>
void inline_each( Function& caller, const Literal_pool& literals, Pick pick )
{
    std::vector<Block_id>                     worklist( caller.blocks.size() );
    std::vector<Block_id>                     origin( caller.blocks.size() );
    std::vector<std::vector<const Function*>> within( caller.blocks.size() ); // the splices a block came from

    for( const Block_id i : caller.blocks.ids() )
    {
        worklist[i.v] = i;
        origin[i.v]   = i;
    }

    while( !worklist.empty() )
    {
        const Block_id at = worklist.back();
        worklist.pop_back();

        Block& block = caller.blocks[at];

        for( u32 s = 0; s < block.statement_count; ++s )
        {
            const Site site      = pick( at, s, origin[at.v] );
            const auto recursive = [&]( const Function& f )
            { return std::ranges::any_of( f.statements, [&]( const Statement& t ) { return calls( f, t, f ); } ); };
            if( site.callee && std::ranges::find( within[at.v], site.callee ) == within[at.v].end() &&
                ( within[at.v].empty() || !recursive( *site.callee ) ) )
            {
                const u32 block_base = narrow_cast<u32>( caller.blocks.size() );
                splice( caller, at, s, *site.callee, site.receiver );
                origin.resize( caller.blocks.size(), origin[at.v] );
                std::vector<const Function*> inner = within[at.v];
                inner.push_back( site.callee );
                within.resize( caller.blocks.size(), inner );
                within.back() = within[at.v]; // the continuation is the caller's own
                for( u32 b = block_base; b < caller.blocks.size(); ++b )
                {
                    worklist.push_back( Block_id { b } );
                }
                break;
            }
        }
    }

    remove_statements( caller, std::vector<bool>( caller.statements.size(), false ) );
    simplify( caller, literals );
}

} // namespace

void inline_calls(
    Function&                                   caller,
    const std::vector<std::optional<Function>>& saved,
    const std::vector<Function>&                functions,
    const Type_table&                           types,
    Literal_pool&                               literals,
    const Purity&                               purity,
    bool                                        keep_trig,
    const Inline_weights&                       weights
)
{
    const u32 statement_count = narrow_cast<u32>( caller.statements.size() );

    // A call run many times pays its overhead each time, so in a loop a dearer callee goes.
    std::vector<bool> looped( caller.blocks.size(), false );
    for( const Loop& loop : find_loops( caller ).loops )
    {
        for( const Block_id b : loop.blocks )
        {
            looped[b.v] = true;
        }
    }

    inline_each(
        caller,
        literals,
        [&]( Block_id at, u32 s, Block_id from ) -> Site
        {
            const Block&     block     = caller.blocks[at];
            const Statement& statement = caller.statements_of( block )[s];
            const Rvalue&    value     = std::as_const( caller ).rvalue( statement );

            // A call through a local holding a known function is a call to it, inlined or not.
            if( statement.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Indirect_call )
            {
                if( const Rvalue* known = known_function( caller, value.a ) )
                {
                    caller.rvalue( statement ) = call( known->callee, value.arguments, value.type, known->type_arguments );
                }
            }

            if( statement.kind != Statement_kind::Assign || value.kind != Rvalue_kind::Call )
            {
                return {};
            }

            for( u32 j = 0; j < saved.size(); ++j )
            {
                if( !saved[j] )
                {
                    continue;
                }

                if( calls( caller, statement, *saved[j] ) )
                {
                    const Function&      callee   = *saved[j];
                    const Function&      finished = functions[j];
                    const u32            limit    = threshold( statement_count, finished, weights, looped[from.v] );
                    std::optional<Place> receiver;
                    bool                 fits = !( keep_trig && callee.trig != Trig::None ) && inlinable( callee );
                    // Priced first as it stands, then as this call folds it: finished, and as spliced once
                    // every loop goes, as one the call runs once does.
                    fits = fits && ( inline_cost( finished, types, weights ) <= limit ||
                                     folded_cost( finished, caller, at, s, types, literals, purity, weights ) <= limit ||
                                     folded_cost( callee, caller, at, s, types, literals, purity, weights, true ) <= limit );
                    if( fits && callee.constructed.is_valid() )
                    {
                        assert( callee.constructed == Local_id { 1 } );
                        const std::optional<u32> built = initialised( caller, block, s, value.arguments[0] );
                        if( built )
                        {
                            receiver = caller.rvalue( caller.statements_of( block )[*built] ).a.place;
                        }
                        fits = receiver.has_value() && replaceable_receiver( callee );
                    }
                    if( fits )
                    {
                        return Site { .callee = &callee, .receiver = receiver };
                    }
                    else
                    {
                        return {};
                    }
                }
            }

            return {};
        }
    );
}

void inline_drops(
    Function&                    caller,
    const std::vector<Function>& functions,
    const std::vector<bool>&     finished,
    const Type_table&            types,
    const Literal_pool&          literals,
    const Inline_weights&        weights
)
{
    const u32 statement_count = narrow_cast<u32>( caller.statements.size() );
    inline_each(
        caller,
        literals,
        [&]( Block_id at, u32 s, Block_id ) -> Site
        {
            const Block&     block     = caller.blocks[at];
            const Statement& statement = caller.statements_of( block )[s];
            if( statement.kind != Statement_kind::Drop || statement.drop_flag.is_valid() ||
                !caller.rvalue( statement ).callee.is_valid() )
            {
                return {};
            }

            for( u32 j = 0; j < functions.size(); ++j )
            {
                if( !finished[j] )
                {
                    continue;
                }

                if( calls( caller, statement, functions[j] ) )
                {
                    if( replaceable_receiver( functions[j] ) && inline_cost( functions[j], types, weights ) <=
                                                                    threshold( statement_count, functions[j], weights, false ) )
                    {
                        return Site { .callee = &functions[j], .receiver = statement.place };
                    }
                    else
                    {
                        return {};
                    }
                }
            }

            return {};
        }
    );
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

constexpr std::string_view k_types = "class Box { public i64 value; public Box( i64 v ) { value = v; } ~Box() { } };\n"
                                     "i32 twice( i32 x ) { return x * 2; }\n"
                                     "void set( out i32 n ) { n = 9; }\n"
                                     "i32 mixed( i32 a, i32 b ) { i32 c = a * b + a; i32 d = c - b * 3; i32 e = d * d + c;"
                                     " i32 f = e / 7 + d; i32 g = f * a - e; i32 h = g * b + f * c; i32 i = h * h + g;"
                                     " i32 j = i / 5 - h; i32 k = j * c + i; i32 l = k - j * d; i32 m = l * e + k;"
                                     " i32 n = m * f + l; i32 o = n - m * g; return o + h * i - l; }\n"
                                     "i32 main() { return 0; }\n";

// Optimised and elaborated, as a release build emits it. The function under test is always the first.
struct Inlined : Compiled
{
    explicit Inlined( std::string_view source, Inline_weights weights = {}, u32 rounds = 2 )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build {
                  .level    = 2,
                  .inlining = weights,
                  .loops    = rounds < 2 ? std::nullopt : std::optional( Loop_weights {} ),
                  .rounds   = rounds
              }
          )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    // Direct calls left in the function under test.
    std::size_t calls() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s )
            { return s.kind == Statement_kind::Assign && first().rvalue( s ).kind == Rvalue_kind::Call; }
        ) );
    }

    std::size_t indirect_calls() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s )
            { return s.kind == Statement_kind::Assign && first().rvalue( s ).kind == Rvalue_kind::Indirect_call; }
        ) );
    }

    std::size_t drops( bool flagged ) const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s ) { return s.kind == Statement_kind::Drop && s.drop_flag.is_valid() == flagged; }
        ) );
    }
};

} // namespace

TEST_CASE( "inline_calls_inlines_both_calls_in_one_block", "[opt][inline]" )
{
    const Inlined p( "i32 f( i32 a ) { return twice( a ) + twice( a + 1 ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 0 );
}

// The call inside `f` is into its own group, so the body never takes itself.
TEST_CASE( "inline_calls_keeps_a_recursive_call", "[opt][inline]" )
{
    const Inlined p( "i64 f( i64 n ) { if( n <= 1 ) { return 1; } return n * f( n - 1 ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 1 );
}

// The weights are the knobs: raise the threshold and the same call goes.
TEST_CASE( "inline_calls_keeps_a_call_over_the_threshold", "[opt][inline]" )
{
    const std::string_view source = "i32 f( i32 a ) { return mixed( a, 3 ); }\n";

    const Inlined kept( source );
    REQUIRE( kept.clean() );
    REQUIRE( kept.calls() == 1 );

    const Inlined raised( source, Inline_weights { .threshold = 100 } );
    REQUIRE( raised.clean() );
    REQUIRE( verify( raised.first() ).empty() );
    REQUIRE( raised.calls() == 0 );
}

// A call inside a loop may cost `in_loop` more; the same call outside one stays.
TEST_CASE( "inline_calls_takes_a_dearer_callee_in_a_loop", "[opt][inline]" )
{
    const Inline_weights weights { .in_loop = 100 };

    const Inlined straight( "i32 f( i32 a ) { return mixed( a, 3 ); }\n", weights );
    REQUIRE( straight.clean() );
    REQUIRE( straight.calls() == 1 );

    const Inlined looped(
        "i32 f( i32 a ) { i32 s = 0; for( i32 i = 0; i < a; i += 1 ) { s = s + mixed( i, 3 ); } return s; }\n", weights
    );
    REQUIRE( looped.clean() );
    REQUIRE( verify( looped.first() ).empty() );
    REQUIRE( looped.calls() == 0 );
}

// `mixed`'s body behind a test of `k`: dear as it stands, cheap once a constant `k` folds the test.
constexpr std::string_view k_chosen =
    "i32 chosen( i32 k, i32 a ) { if( k == 0 ) { return a + 1; }"
    " i32 c = a * k + a; i32 d = c - k * 3; i32 e = d * d + c; i32 f = e / 7 + d;"
    " i32 g = f * a - e; i32 h = g * k + f * c; i32 i = h * h + g; i32 j = i / 5 - h;"
    " i32 l = j * c + i; i32 m = l * e + j; i32 n = m * f + l; i32 o = n - m * g; return o + h * i - l; }\n";

TEST_CASE( "inline_calls_prices_a_callee_as_its_constant_arguments_fold_it", "[opt][inline]" )
{
    const Inlined folded( "i32 f( i32 a ) { return chosen( 0, a ); }\n" + std::string( k_chosen ) );
    REQUIRE( folded.clean() );
    REQUIRE( verify( folded.first() ).empty() );
    REQUIRE( folded.calls() == 0 );

    const Inlined unknown( "i32 f( i32 a, i32 k ) { return chosen( k, a ); }\n" + std::string( k_chosen ) );
    REQUIRE( unknown.clean() );
    REQUIRE( unknown.calls() == 1 );
}

// The same for a run's size: `v` makes a run of 3, so the dear arm never runs.
TEST_CASE( "inline_calls_prices_a_callee_as_a_run_of_known_size_folds_it", "[opt][inline]" )
{
    const std::string sized = "i32 sized( const ref i32[] r ) { if( r.size == 3 ) { return r[0]; }"
                              " i32 a = r[0]; i32 k = r[1]; i32 c = a * k + a; i32 d = c - k * 3; i32 e = d * d + c;"
                              " i32 f = e / 7 + d; i32 g = f * a - e; i32 h = g * k + f * c; i32 i = h * h + g;"
                              " i32 j = i / 5 - h; i32 l = j * c + i; i32 m = l * e + j; i32 n = m * f + l; i32 o = n - m * g;"
                              " return o + h * i - l; }\n";

    const Inlined known( "i32 f( i32 a ) { i32[3] v = [ a, 2, 3 ]; return sized( v ); }\n" + sized );
    REQUIRE( known.clean() );
    REQUIRE( verify( known.first() ).empty() );
    REQUIRE( known.calls() == 0 );

    const Inlined unknown( "i32 f( const ref i32[] r ) { return sized( r ); }\n" + sized );
    REQUIRE( unknown.clean() );
    REQUIRE( unknown.calls() == 1 );
}

// `mix` reads and writes through its `ref`s, which may name one place, so as it stands every step
// goes through memory; given two distinct locals, the steps are plain arithmetic and it fits.
constexpr std::string_view k_mix =
    "u64 spin( u64 x ) { return ( x << 13 ) ^ ( x >> 7 ); }\n"
    "void mix( ref u64 a, ref u64 b ) { a = wrap<u64>( a + b ); b = spin( b ); b ^= a; a = spin( a );"
    " a = wrap<u64>( a + b ); b = spin( b ); b ^= a; a = spin( a ); a = wrap<u64>( a + b ); b = spin( b ); b ^= a; }\n";

TEST_CASE( "inline_calls_prices_a_callee_as_distinct_borrowed_locals_leave_it", "[opt][inline]" )
{
    const Inlined distinct(
        "u64 f( u64 x, u64 y ) { u64 a = x; u64 b = y; mix( ref a, ref b ); return a + b; }\n" + std::string( k_mix )
    );
    REQUIRE( distinct.clean() );
    REQUIRE( verify( distinct.first() ).empty() );
    REQUIRE( distinct.calls() == 0 );

    const Inlined same( "u64 f( u64 x ) { u64 a = x; mix( ref a, ref a ); return a; }\n" + std::string( k_mix ) );
    REQUIRE( same.clean() );
    REQUIRE( same.calls() == 1 );
}

// SipHash's round takes its four words by `ref` and rotates them: bound to `write`'s and `finish`'s
// locals, with each rotate one statement, it fits even outside a loop, so no call of it is left.
TEST_CASE( "inline_calls_takes_the_hashers_rounds", "[opt][inline]" )
{
    const Inlined p( "u64 f( u64 x ) { hasher h = hasher( 1, 2 ); h.write( x ); return h.finish(); }\n" );
    REQUIRE( p.clean() );
    std::size_t rounds = 0;
    for( const Function& func : p.all )
    {
        for( const Statement& s : func.statements )
        {
            rounds += s.kind == Statement_kind::Assign && func.rvalue( s ).kind == Rvalue_kind::Call &&
                      p.interner.text( p.ast.name( func.rvalue( s ).callee ) ) == "round";
        }
    }
    REQUIRE( rounds == 0 );
}

// `pick` loops over its indices, so as it stands it is dear; called with one, the rotated loop's latch
// folds, and only the one turn is priced: within a loop's threshold, as a sort's comparator is, and
// over a straight call's.
TEST_CASE( "inline_calls_prices_a_loop_the_call_runs_once_as_straight_code", "[opt][inline]" )
{
    const std::string pick =
        "i64 pick( const ref i64[] v, const ref u64[] shape, u64... at ) { u64 flat = 0; for( u64 k : 0..at.size ) {"
        " if( at[k] >= shape[k] ) { panic( \"index out of range\" ); } u64 m = at[k] * 7 + 3; u64 n = m ^ ( m >> 2 ); flat = "
        "flat * shape[k] + at[k] + ( n & 1 ) - ( n & 1 ); }"
        " return v[flat]; }\n";

    const Inlined once(
        "i64 f( const ref i64[] v, const ref u64[] shape, u64 n ) { i64 s = 0; for( u64 i : 0..n ) { s += pick( v, shape, i ); "
        "} return s; }\n" +
        pick
    );
    REQUIRE( once.clean() );
    REQUIRE( verify( once.first() ).empty() );
    REQUIRE( once.calls() == 0 );

    const Inlined straight(
        "i64 f( const ref i64[] v, const ref u64[] shape, u64 i ) { return pick( v, shape, i ); }\n" + pick
    );
    REQUIRE( straight.clean() );
    REQUIRE( straight.calls() == 1 );
}

// `cross3`'s element addresses are addressing modes, so only its arithmetic counts and it fits.
TEST_CASE( "inline_calls_prices_an_element_address_as_free", "[opt][inline]" )
{
    const std::string source =
        "i32 f( i32 x ) { i32[3] a = [ x, 2, 3 ]; i32[3] b = [ 3, x, 1 ]; i32[3] d = [ 0, 0, 0 ];"
        " cross3( ref d, a, b ); return d[0] + d[1] + d[2]; }\n"
        "void cross3( ref i32[] d, const ref i32[] a, const ref i32[] b ) {"
        " if( a.size != 3 || b.size != 3 || d.size != 3 ) { panic( \"size\" ); }"
        " d[0] = a[1] * b[2] - a[2] * b[1]; d[1] = a[2] * b[0] - a[0] * b[2]; d[2] = a[0] * b[1] - a[1] * b[0]; }\n";

    const Inlined free( source );
    REQUIRE( free.clean() );
    REQUIRE( verify( free.first() ).empty() );
    REQUIRE( free.calls() == 0 );

    const Inlined counted( source, Inline_weights { .address = 1 } );
    REQUIRE( counted.calls() == 1 );
}

// `pass` keeps its call of `mixed`, too dear outside a loop; spliced into `f`'s loop, the call is offered
// again at the loop's limit and goes too.
// `total`'s checks read `i * cols` as an induction stepped by `cols`; called with constant sizes in a loop,
// they fold against its counter, which leaves it cheap enough.
TEST_CASE( "inline_calls_prices_a_reduced_index_as_its_counter_bounds_it", "[opt][inline]" )
{
    const std::string source =
        "i64 f( const ref i64[16] m, u64 n ) { i64 t = 0; for( u64 r = 0; r < n; r += 1 ) { t += total( m, 4, 4 ); } return t; "
        "}\n"
        "i64 total( const ref i64[] a, u64 rows, u64 cols ) { if( a.size != rows * cols ) { panic( \"shape\" ); } i64 s = 0;"
        " for( u64 i : 0..rows ) { for( u64 j : 0..cols ) { s += a[i * cols + j]; } } return s; }\n";
    const Inlined p( source, Inline_weights { .threshold = 13, .in_loop = 0 } );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 0 );
}

TEST_CASE( "inline_calls_offers_a_spliced_call_again_at_its_new_site", "[opt][inline]" )
{
    const std::string passing = "i32 pass( i32 x, i32 y ) { return mixed( x, y ); }\n";
    const Inlined     looped(
        "i32 f( i32 n, i32 y ) { i32 t = 0; for( i32 i = 0; i < n; i++ ) { t = t + pass( i, y ); } return t; }\n" + passing
    );
    REQUIRE( looped.clean() );
    REQUIRE( verify( looped.first() ).empty() );
    REQUIRE( looped.calls() == 0 );

    const Inlined once( "i32 f( i32 n, i32 y ) { return pass( n, y ); }\n" + passing );
    REQUIRE( once.clean() );
    REQUIRE( once.calls() == 1 );
}

// Both are reached through `&init`, whose pointer the passes trust to die with its call.
TEST_CASE( "inline_calls_keeps_a_call_that_initialises_through_out", "[opt][inline]" )
{
    const Inlined p( "i32 f() { i32 n = 0; set( out n ); return n; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 1 );
}

// `(*this).value` becomes `b.value`, so `b` is built where it lies.
TEST_CASE( "inline_calls_inlines_a_constructor", "[opt][inline]" )
{
    const Inlined p( "i64 f() { Box b = Box( 4 ); return b.value; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 0 );
}

// The callee's `p` arrives with its fields; the inlined constructor's receiver, a pointer, brings none.
// No rounds, so `q` is not folded away, and its slots with it.
TEST_CASE( "inline_calls_carries_the_callees_field_table", "[opt][inline]" )
{
    const Inlined p(
        "i32 f() { Box b = Box( 4 ); return sum(); }\n"
        "struct Pr { i32 x; i32 y; };\n"
        "i32 sum() { Pr q = Pr { 1, 2 }; return q.x + q.y; }\n",
        {},
        0
    );
    REQUIRE( p.clean() );
    REQUIRE( p.calls() == 0 );

    const Function& func = p.first();
    std::size_t     xs   = 0;

    for( u32 i = 0; i < func.fields.size(); ++i )
    {
        const Field_slot& slot = func.fields[i];
        REQUIRE( slot.local.v < func.locals.size() );
        REQUIRE( slot.end > i );
        REQUIRE( slot.end <= func.fields.size() );

        if( slot.parent == k_no_field )
        {
            const Type_id type = func.locals[slot.local].type;
            REQUIRE( p.types.table().is_struct( type ) );
            REQUIRE( p.ast.enclosing_aggregate( slot.field ) == p.types.table().get( type ).declaration );
        }
        else
        {
            REQUIRE( slot.parent < i );
        }

        xs += p.ast.name( slot.field ) == p.interner.find( "x" ) ? 1 : 0;
    }

    REQUIRE( xs > 0 ); // `q`'s, and the literal's temporary's
}

// `fill` is over the threshold and takes `this` itself, which no place can stand in for.
TEST_CASE( "inline_calls_keeps_a_constructor_that_passes_this_on", "[opt][inline]" )
{
    const Inlined p( "i32 f() { Reg r = Reg( 4 ); return r.value; }\n"
                     "class Reg { public i32 value; public Reg( i32 v ) { value = v; fill(); }"
                     " void fill() { value = mixed( value, 2 ) + mixed( value, 4 ) + mixed( value, 6 ) + mixed( value, 8 )"
                     " + mixed( value, 1 ) + mixed( value, 3 ); } };\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 1 );
}

// Weighed as lowered, `Two()` is over the threshold: each assignment drops the field it replaces.
// Its own passes leave two stores, which is what its callers weigh; and no local is left behind
// that only storage markers name.
TEST_CASE( "inline_calls_weighs_a_callee_as_its_passes_leave_it", "[opt][inline]" )
{
    const Inlined p( "i64 f() { Two t = Two(); return t.a.value + t.b.value; }\n"
                     "class Two { public Box a; public Box b; public Two() { a = Box( 1 ); b = Box( 2 ); } };\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.calls() == 0 );

    for( const Function& function : p.functions )
    {
        std::vector<bool> named( function.locals.size(), false );
        for( u32 l = 0; l <= function.parameter_count; ++l )
        {
            named[l] = true;
        }
        for_each_place(
            function,
            [&]( const Place& place, const Statement* statement )
            {
                if( place.local.is_valid() && ( !statement || ( statement->kind != Statement_kind::Storage_live &&
                                                                statement->kind != Statement_kind::Storage_dead ) ) )
                {
                    named[place.local.v] = true;
                }
            }
        );
        for( const Statement& statement : function.statements )
        {
            if( statement.drop_flag.is_valid() )
            {
                named[statement.drop_flag.v] = true;
            }
        }
        REQUIRE( std::ranges::all_of( named, []( bool n ) { return n; } ) );
    }
}

// A local holding `&twice`, its own or an inlined callee's parameter, calls `twice`, which then inlines.
TEST_CASE( "inline_calls_inlines_a_call_through_a_known_function", "[opt][inline]" )
{
    for( const char* source : {
             "i32 f( i32 a ) { fn( i32 ) -> i32 g = &twice; return g( a ); }\n",
             "i32 f( i32 a ) { return apply( &twice, a ); }\n"
             "i32 apply( fn( i32 ) -> i32 g, i32 x ) { return g( x ); }\n",
         } )
    {
        const Inlined p( source );
        INFO( source );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.first() ).empty() );
        REQUIRE( p.calls() == 0 );
        REQUIRE( p.indirect_calls() == 0 );
    }
}

// Assigned twice, the local may hold either.
TEST_CASE( "inline_calls_keeps_a_call_through_a_reassigned_local", "[opt][inline]" )
{
    const Inlined p( "i32 f( i32 a ) { fn( i32 ) -> i32 g = &twice; if( a > 0 ) { g = &thrice; } return g( a ); }\n"
                     "i32 thrice( i32 x ) { return x * 3; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.indirect_calls() == 1 );
}

TEST_CASE( "inline_drops_inlines_an_unconditional_drop", "[opt][inline]" )
{
    const Inlined p( "i64 f() { Box b = Box( 4 ); return b.value; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( false ) == 0 );
}

// Whether `~Box` runs is only known at run time, so the drop stays.
TEST_CASE( "inline_drops_keeps_a_flagged_drop", "[opt][inline]" )
{
    const Inlined p( "i64 f( bool c ) { Box b = Box( 3 ); if( c ) { consume( move b ); } return 0; }\n"
                     "void consume( move Box b ) { }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.drops( true ) == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
