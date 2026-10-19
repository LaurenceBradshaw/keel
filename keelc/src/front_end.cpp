// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "front_end.h"
#include "check/drop_flags.h"
#include "check/report.h"
#include "ir/lower.h"
#include "ir/simplify.h"

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
    front.functions = lower( ast, front.resolution, front.types, literals );

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

    const Literal_id debug = build == Build::Debug ? vocabulary.true_literal : vocabulary.false_literal;

    // Settled only after the checks, so they saw both paths; simplified again so elaboration sees
    // only this build's.
    for( Function& function : front.functions )
    {
        settle_debug( function, debug );
        simplify( function, literals );
        elaborate_drops( function, vocabulary );
    }

    return front;
}

} // namespace keel
