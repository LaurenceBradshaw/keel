#include "check/assign_check.h"

namespace keel
{

namespace
{

// A *must* analysis, which is the whole reason this is not a question inside check_moves. There the
// bottom is "no information" and merging raises a local up the lattice; here the bottom is "assigned
// everywhere" and merging can only lower it, because a value is definitely assigned only if every
// path assigned it.
//
// `reached` is what keeps that honest. A block the worklist has not visited yet holds the identity -
// all assigned - and intersecting with it would be a lie, so it is skipped until something reaches
// it. `ever` is the same information joined the other way, and exists only so the diagnostic can
// say "on some paths" rather than "never".
struct Flow
{
    std::vector<u8> always; // assigned on every path into here
    std::vector<u8> ever;   // assigned on at least one
    bool            reached = false;
};

Flow bottom( std::size_t locals )
{
    return Flow { std::vector<u8>( locals, 1 ), std::vector<u8>( locals, 0 ), false };
}

// Merges `from` into `into`, returning whether anything changed - the worklist's only termination
// signal, exactly as in check_moves.
bool merge_into( Flow& into, const Flow& from )
{
    if( !into.reached )
    {
        into = from;
        return true;
    }

    bool changed = false;

    for( std::size_t i = 0; i < into.always.size(); ++i )
    {
        const u8 always = static_cast<u8>( into.always[i] && from.always[i] );
        const u8 ever   = static_cast<u8>( into.ever[i] || from.ever[i] );

        changed = changed || always != into.always[i] || ever != into.ever[i];

        into.always[i] = always;
        into.ever[i]   = ever;
    }

    return changed;
}

// 0, 1 or 2 of them, by terminator kind. Deliberately the same shape as check_moves', because it is
// the same question about the same graph.
void successors( const Terminator& terminator, std::vector<Block_id>& out )
{
    switch( terminator.kind )
    {
    case Terminator_kind::Goto:
        out.push_back( terminator.targets[0] );
        return;
    case Terminator_kind::Branch:
        out.push_back( terminator.targets[0] );
        out.push_back( terminator.targets[1] );
        return;
    case Terminator_kind::Unset:
    case Terminator_kind::Return:
    case Terminator_kind::Unreachable:
        return;
    }
}

// Only an Assign matters. A projected target counts: `(*_1).x = ...` is a step in building the
// referent, and an `out` parameter is written exactly that way when its type is a struct.
//
// Storage_dead clears, so a local reused after its scope ends does not carry the old answer. It
// cannot name an `out` parameter - those live for the whole function - but the pass is over locals
// rather than over parameters, and a rule that holds for one should hold for all of them.
void transfer_block( const Function& func, u32 block, Flow& flow )
{
    const Block& b = func.blocks[block];

    for( u32 i = 0; i < b.statement_count; ++i )
    {
        const Statement& statement = func.statements[b.first_statement + i];

        if( statement.place.is_global() )
        {
            continue;
        }

        const u32 local = statement.place.local.v;

        switch( statement.kind )
        {
        case Statement_kind::Assign:
            flow.always[local] = 1;
            flow.ever[local]   = 1;
            break;
        case Statement_kind::Storage_dead:
            flow.always[local] = 0;
            flow.ever[local]   = 0;
            break;
        case Statement_kind::Storage_live:
        case Statement_kind::Drop:
            break;
        }
    }
}

Flow entry_flow( const Function& func )
{
    Flow flow { std::vector<u8>( func.locals.size(), 1 ), std::vector<u8>( func.locals.size(), 1 ), true };

    // The one thing this pass asserts about the entry: an `out` parameter's referent is empty until
    // the body writes it. Everything else is taken as assigned, because this pass is not general
    // definite assignment - it answers one question, and a local nobody made it look at must not
    // become an error by accident.
    for( const Local_id local : func.out_parameters )
    {
        flow.always[local.v] = 0;
        flow.ever[local.v]   = 0;
    }

    return flow;
}

} // namespace

std::vector<Unassigned_error> check_assignment( const Function& func )
{
    std::vector<Unassigned_error> errors;

    if( func.out_parameters.empty() )
    {
        return errors; // nothing to be unassigned, and the fixpoint would prove it slowly
    }

    std::vector<Flow> in( func.blocks.size(), bottom( func.locals.size() ) );
    in[0] = entry_flow( func );

    std::vector<Block_id> work { Block_id { 0 } };
    std::vector<Block_id> next;

    // Forward, pushing into successors rather than pulling from predecessors, so no predecessor map
    // is needed. Terminates because a local's `always` only ever falls and its `ever` only ever
    // rises, both are one bit, and there are finitely many blocks - so merge_into eventually
    // returns false everywhere and the worklist drains.
    while( !work.empty() )
    {
        const Block_id block = work.back();
        work.pop_back();

        Flow out = in[block.v];
        transfer_block( func, block.v, out );

        next.clear();
        successors( func.blocks[block.v].terminator, next );

        for( const Block_id target : next )
        {
            if( merge_into( in[target.v], out ) )
            {
                work.push_back( target );
            }
        }
    }

    // Every return is a way out, so every return has to have written them. Reported once per block
    // rather than inside the loop above, which would emit one error per visit for any block on a
    // back edge - the same reason check_moves reports in a second walk.
    for( u32 block = 0; block < func.blocks.size(); ++block )
    {
        const Block& b = func.blocks[block];

        if( b.terminator.kind != Terminator_kind::Return || !in[block].reached )
        {
            continue;
        }

        Flow flow = in[block];
        transfer_block( func, block, flow );

        for( const Local_id local : func.out_parameters )
        {
            if( flow.always[local.v] == 0 )
            {
                errors.push_back( Unassigned_error { .local = local, .at = b.terminator.span, .maybe = flow.ever[local.v] != 0 }
                );
            }
        }
    }

    return errors;
}

} // namespace keel
