// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "front_end.h"
#include "check/drop_flags.h"
#include "check/report.h"
#include "ir/call_graph.h"
#include "ir/lower.h"
#include "ir/simplify.h"
#include "ir/verify.h"
#include "opt/coalesce_moves.h"
#include "opt/collapse_borrows.h"
#include "opt/inline_calls.h"
#include "opt/merge_blocks.h"
#include "opt/propagate_constants.h"
#include "opt/propagate_values.h"
#include "opt/remove_dead_stores.h"
#include "opt/settle_drops.h"
#include "opt/split_aggregates.h"
#include "opt/thread_jumps.h"

namespace keel
{

Front_end run_front_end(
    File_id                  input,
    Stage                    through,
    Build                    build,
    Source_manager&          sm,
    Interner&                interner,
    Literal_pool&            literals,
    Diagnostics&             diags,
    std::span<const Package> packages
)
{
    Front_end front;
    front.program  = load_program( input, sm, interner, literals, diags, packages );
    const Ast& ast = front.program.ast;

    front.resolution = resolve( ast, sm, interner, diags, front.program.imports );
    front.types      = type_check( ast, front.resolution, literals, sm, interner, diags );

    // The emitter takes no Diagnostics: past here there is nothing left for it to object to.
    if( diags.has_errors() )
    {
        return front;
    }

    // Lowered once, for everything downstream: the move check, --dump-kir and the emitter all read
    // the same functions.
    front.functions = lower( ast, front.resolution, front.types, literals, sm, build.overflow_checks );

    if( through == Stage::Lowered )
    {
        return front;
    }

    for( Function& function : front.functions )
    {
        simplify( function, literals );
    }

    if( through == Stage::Simplified )
    {
        return front;
    }

    // Part of the front end, not of emission: --check is what an editor wants, and an editor wants
    // use-after-move underlined.
    report_dataflow_errors( front.functions, ast, sm, interner, front.types, diags );

    if( through == Stage::Reported || diags.has_errors() )
    {
        return front;
    }

    // Built once rather than per function: the two literals are interned.
    const Flag_vocabulary vocabulary {
        .bool_type     = front.types.table().builtin( Type_kind::Bool ),
        .false_literal = literals.add_integer( 0 ),
        .true_literal  = literals.add_integer( 1 )
    };

    const Literal_id debug = build.asserts ? vocabulary.true_literal : vocabulary.false_literal;

    // Settled only after the checks, so they saw both paths; simplified again so elaboration sees
    // only this build's. A release build optimises before elaboration makes drops explicit and again
    // after, splitting aggregates first.
    // Callees first, so a callee's body is ready when its callers inline it.
    std::vector<std::optional<Function>> saved( front.functions.size() ); // each function as its callers inline it
    std::vector<bool> finished( front.functions.size(), false ); // each function once optimised, as its droppers inline it

    for( const std::vector<u32>& group : callees_first( front.functions ) )
    {
        for( const u32 f : group )
        {
            Function& function = front.functions[f];

            settle_debug( function, debug );
            simplify( function, literals );

            if( build.optimise )
            {
                if( build.inlining )
                {
                    inline_calls( function, saved, literals, *build.inlining );
                }
                collapse_borrows( function, front.types.table() );
                coalesce_moves( function );
                assert( verify( function ).empty() );
            }
        }

        // Saved only once the whole group has inlined, so no member inlines another: that is what
        // stops recursion.
        if( build.optimise && build.inlining )
        {
            for( const u32 f : group )
            {
                saved[f] = front.functions[f];
            }
        }

        for( const u32 f : group )
        {
            Function& function = front.functions[f];

            elaborate_drops( function, vocabulary );

            if( build.optimise )
            {
                settle_drops( function );
                if( build.inlining )
                {
                    inline_drops( function, front.functions, finished, literals, *build.inlining );
                }
                if( build.splitting )
                {
                    split_aggregates( function, ast, front.types );
                }
                // Folds a local compared with itself, so SCCP branches on the constant.
                propagate_values( function, front.types.table(), literals );
                propagate_constants( function, front.types.table(), literals );
                thread_jumps( function, front.types.table(), literals );
                // Again, as a join SCCP left fewer paths into may now agree.
                propagate_values( function, front.types.table(), literals );
                remove_dead_stores( function );
                // Dead store removal can empty a block, so threading goes first.
                simplify( function, literals );
                merge_blocks( function );
                assert( verify( function ).empty() );
            }
        }

        if( build.optimise && build.inlining )
        {
            for( const u32 f : group )
            {
                finished[f] = true;
            }
        }
    }

    return front;
}

} // namespace keel
