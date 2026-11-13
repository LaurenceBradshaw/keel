// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "front_end.h"
#include <fmt/core.h>
#include <algorithm>
#include "check/drop_flags.h"
#include "check/report.h"
#include "ir/call_graph.h"
#include "ir/lower.h"
#include "ir/lower_dispatch.h"
#include "ir/main_shim.h"
#include "ir/purity.h"
#include "ir/simplify.h"
#include "ir/verify_types.h"
#include "opt/coalesce_moves.h"
#include "opt/collapse_borrows.h"
#include "opt/devirtualise_calls.h"
#include "opt/eliminate_common.h"
#include "opt/fold_constants.h"
#include "opt/forward_borrows.h"
#include "opt/forward_globals.h"
#include "opt/forward_reads.h"
#include "opt/hoist_invariants.h"
#include "opt/inline_calls.h"
#include "opt/loop_self_calls.h"
#include "opt/merge_blocks.h"
#include "opt/propagate_constants.h"
#include "opt/propagate_values.h"
#include "opt/reduce_inductions.h"
#include "opt/remove_dead_stores.h"
#include "opt/settle_drops.h"
#include "opt/shadow_tags.h"
#include "opt/solve_linear_maps.h"
#include "opt/split_aggregates.h"
#include "opt/thread_jumps.h"
#include "opt/unroll_loops.h"

namespace keel
{

void Phase_clock::lap( std::string_view phase )
{
    const Clock::time_point now = Clock::now();
    if( print_ )
    {
        fmt::print( stderr, "keelc: {:<16} {:.2f}s\n", phase, std::chrono::duration<double>( now - last_ ).count() );
    }
    last_ = now;
}

void Phase_clock::note( std::string_view line ) const
{
    if( print_ )
    {
        fmt::print( stderr, "keelc: {}\n", line );
    }
}

double Phase_clock::seconds() const
{
    return std::chrono::duration<double>( Clock::now() - start_ ).count();
}

Front_end run_front_end(
    File_id                  input,
    Stage                    through,
    Build                    build,
    Source_manager&          sm,
    Interner&                interner,
    Literal_pool&            literals,
    Diagnostics&             diags,
    std::span<const Package> packages,
    bool                     as_module,
    std::span<const Object>  objects,
    std::string_view         prelude,
    std::string_view         prelude_name,
    Phase_clock*             clock
)
{
    const auto lap = [&]( std::string_view phase )
    {
        if( clock != nullptr )
        {
            clock->lap( phase );
        }
    };

    Front_end front;
    front.program = load_program( input, sm, interner, literals, diags, packages, as_module, objects, prelude, prelude_name );
    lap( "load" );
    const Ast& ast = front.program.ast;

    const std::span<const Loaded_state> states = front.program.states;
    front.resolution                           = resolve( ast, sm, interner, diags, front.program.modules, states );
    lap( "resolve" );
    front.types = type_check( ast, front.resolution, literals, sm, interner, diags, std::move( front.program.types ), states );
    lap( "type check" );

    // The emitter takes no Diagnostics: past here there is nothing left for it to object to.
    if( diags.has_errors() )
    {
        return front;
    }

    // The loaded modules' functions come first, as their objects hold them: checked already, and in a
    // debug build compiled already. One instance two objects hold is kept once, from the first.
    std::vector<Function> loaded;
    for( const Loaded_state& state : states )
    {
        for( const Function& function : state.functions )
        {
            if( function.globals.is_valid() ||
                std::ranges::none_of( loaded, [&]( const Function& kept ) { return same_function( kept, function ); } ) )
            {
                loaded.push_back( function );
                front.provided.push_back( state.file );
            }
        }
    }
    const std::size_t given = loaded.size();

    for( const Loaded_state& state : states )
    {
        if( state.level == build.level )
        {
            front.reused.push_back( state.file );
        }
    }
    // A module's object holds only its own part, so the loaded ones are never optimised for it.
    std::vector<bool> kept( given );
    for( std::size_t f = 0; f < given; ++f )
    {
        kept[f] = as_module || front.reuses( front.provided[f] );
    }

    // Lowered once, for everything downstream: the move check, --dump-kir and the emitter all read
    // the same functions.
    front.functions =
        lower( ast, front.resolution, front.types, literals, sm, interner, build.overflow_checks, std::move( loaded ) );
    lap( "lower" );

    // A debug keelc checks what each pass leaves, so a malformed statement names the pass that made it.
    const Type_verifier verifier( ast, front.types, interner, front.functions );
    const auto          verified = [&]( [[maybe_unused]] const Function& function, [[maybe_unused]] std::string_view pass )
    {
#ifndef NDEBUG
        verifier.require( function, pass );
#endif
    };

#ifndef NDEBUG
    for( const Function& function : std::span( front.functions ).subspan( given ) )
    {
        verifier.require_types( function, "lowering" );
    }
#endif

    if( through == Stage::Lowered )
    {
        return front;
    }

    for( Function& function : std::span( front.functions ).subspan( given ) )
    {
        simplify( function, literals );
        verified( function, "simplify" );
    }
    lap( "simplify" );

    if( through == Stage::Simplified )
    {
        return front;
    }

    // Part of the front end, not of emission: --check is what an editor wants, and an editor wants
    // use-after-move underlined.
    // D80: over every function, loaded ones included, since a loaded callee may write a global.
    const std::vector<Global_effects> globals = global_effects_of( front.functions, ast, front.types );
    report_dataflow_errors(
        std::span( front.functions ).subspan( given ),
        std::span( globals ).subspan( given ),
        ast,
        sm,
        interner,
        literals,
        front.types,
        diags
    );
    lap( "dataflow checks" );

    if( build.keep_checked && !diags.has_errors() )
    {
        front.checked = front.functions;
    }

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
    // only this build's. Above -O0 the passes run before elaboration makes drops explicit and again
    // after, splitting aggregates first.
    // Callees first, so a callee's body is ready when its callers inline it.
    std::vector<std::optional<Function>> saved( front.functions.size() ); // each function as its callers inline it
    std::vector<bool> finished( front.functions.size(), false ); // each function once optimised, as its droppers inline it

    // Decided a group at a time as the loop below finishes it, so a caller sees its callees'.
    Purity purity( front.functions, ast );

    // At -O1 a named local keeps its stores, so a debugger finds its current value.
    const bool pin_named = build.level == 1;

    // Comparisons of two locals fold from -O1, beside the loop phase.
    const bool relating = build.level >= 1;

    // The passes after drop elaboration, run `build.rounds` times and once more after the loop phase.
    const auto optimise_round = [&]( Function& function )
    {
        // First, so the local it frees is split and propagated this round.
        if( build.forwarding )
        {
            forward_borrows( function, front.types.table(), pin_named );
            verified( function, "forward_borrows" );
        }
        if( build.level >= 2 && build.splitting )
        {
            split_aggregates( function, ast, front.types );
            verified( function, "split_aggregates" );
            // Again, for a borrow of a field that splitting made a whole local.
            if( build.forwarding )
            {
                forward_borrows( function, front.types.table(), pin_named );
                verified( function, "forward_borrows" );
            }
            shadow_tags( function );
            verified( function, "shadow_tags" );
        }
        // Before propagation, which forwards the copies it leaves.
        if( build.level >= 3 && eliminate_common( function, literals, purity ) )
        {
            verified( function, "eliminate_common" );
        }
        // Folds a local compared with itself, so SCCP branches on the constant.
        propagate_values( function, front.types.table(), literals );
        verified( function, "propagate_values" );
        propagate_constants( function, front.types.table(), literals, relating );
        verified( function, "propagate_constants" );
        if( thread_jumps( function, front.types.table(), literals ) )
        {
            verified( function, "thread_jumps" );
            propagate_constants( function, front.types.table(), literals, relating );
            verified( function, "propagate_constants" );
        }
        // Once SCCP has folded the branches it can; SCCP again folds the constants and checks it leaves.
        if( forward_globals( function, front.types.table(), literals, pin_named ) )
        {
            verified( function, "forward_globals" );
            propagate_constants( function, front.types.table(), literals, relating );
            verified( function, "propagate_constants" );
        }
        // Again, as a join SCCP left fewer paths into may now agree.
        propagate_values( function, front.types.table(), literals );
        verified( function, "propagate_values" );
        remove_dead_stores( function, pin_named, purity );
        verified( function, "remove_dead_stores" );
        // Dead store removal can empty a block, so threading goes first.
        simplify( function, literals );
        verified( function, "simplify" );
        merge_blocks( function );
        verified( function, "merge_blocks" );
        // So no marker of a local a pass removed is left for the next round.
        remove_unused_locals( function, pin_named );
        verified( function, "remove_unused_locals" );
        // Once borrows are forwarded, as a borrowed local stops it; the next round tidies its copies.
        if( build.level >= 2 )
        {
            loop_self_calls( function );
            verified( function, "loop_self_calls" );
        }
    };

    // A call made direct, before or after inlining, finds its callee optimised already.
    std::vector<Node_id> warned_maps; // once a declaration, however many instances it has

    const std::vector<std::vector<u32>> dispatched =
        build.level >= 2 ? dispatch_edges( front.functions, ast, front.types ) : std::vector<std::vector<u32>> {};

    for( const std::vector<u32>& group : callees_first( front.functions, dispatched ) )
    {
        for( const u32 f : group )
        {
            // A reused part links from its object; any other is optimised with the rest.
            if( f < given && kept[f] )
            {
                continue;
            }

            Function& function = front.functions[f];

            settle_debug( function, debug );
            simplify( function, literals );
            verified( function, "settle_debug" );

            if( build.level >= 1 )
            {
                if( build.level >= 2 && build.inlining )
                {
                    // Recognition reads a trig call inside `@real_arithmetic`.
                    const bool keep_trig = build.level >= 3 && function.real_arithmetic;
                    inline_calls( function, saved, front.functions, literals, keep_trig, *build.inlining );
                    verified( function, "inline_calls" );
                    // An inlined body's dispatch may now see its implementer.
                    if( devirtualise_calls( function, ast, front.types ) )
                    {
                        verified( function, "devirtualise_calls" );
                        inline_calls( function, saved, front.functions, literals, keep_trig, *build.inlining );
                        verified( function, "inline_calls" );
                    }
                }
                collapse_borrows( function, front.types.table() );
                verified( function, "collapse_borrows" );
                if( build.level >= 2 )
                {
                    coalesce_moves( function, ast );
                    verified( function, "coalesce_moves" );
                }
            }
        }

        // Saved only once the whole group has inlined, so no member inlines another: that is what
        // stops recursion.
        if( build.level >= 2 && build.inlining )
        {
            for( const u32 f : group )
            {
                saved[f] = front.functions[f];
            }
        }

        for( const u32 f : group )
        {
            if( f < given && kept[f] )
            {
                continue;
            }

            Function& function = front.functions[f];

            elaborate_drops( function, vocabulary );
            verified( function, "elaborate_drops" );

            if( build.level >= 1 )
            {
                settle_drops( function );
                verified( function, "settle_drops" );
                if( build.level >= 2 && build.inlining )
                {
                    inline_drops( function, front.functions, finished, literals, *build.inlining );
                    verified( function, "inline_drops" );
                }
                // Twice by default, as a late pass uncovers work for an earlier one; a third round finds none.
                for( u32 round = 0; round < build.rounds; ++round )
                {
                    optimise_round( function );
                }
                // Again, for the copy chains into a field that splitting and inlining leave.
                if( build.recoalescing )
                {
                    if( build.level >= 2 )
                    {
                        coalesce_moves( function, ast );
                        verified( function, "coalesce_moves" );
                    }
                    // Merging two locals can make `a != a`.
                    fold_identities( function, front.types.table(), literals );
                    verified( function, "fold_identities" );
                }
                remove_unused_locals( function, pin_named );
                verified( function, "remove_unused_locals" );
                // The loop passes, and a round to tidy what they leave; §3.3 gives each its level.
                if( build.loops )
                {
                    if( build.level >= 3 && function.real_arithmetic )
                    {
                        const Map_outcome outcome =
                            solve_linear_maps( function, front.functions, purity, front.types.table(), literals, build.cache );
                        if( clock != nullptr && !outcome.ending )
                        {
                            clock->note( fmt::format(
                                "linear map `{}` {}",
                                interner.text( ast.name( function.declaration ) ),
                                outcome.cached ? "from the cache" : "solved"
                            ) );
                        }
                        if( outcome.solved )
                        {
                            verified( function, "solve_linear_maps" );
                            remove_unused_locals( function, pin_named );
                            verified( function, "remove_unused_locals" );
                            // Callers inline the circuit; it has no owning locals, so no drops to elaborate.
                            saved[f] = function;
                        }
                        else if( outcome.ending && std::ranges::find( warned_maps, function.declaration ) == warned_maps.end() )
                        {
                            warned_maps.push_back( function.declaration );
                            const Span at =
                                outcome.ending->span.is_valid() ? outcome.ending->span : ast.name_span( function.declaration );
                            diags.warning(
                                Warning::Linear_map,
                                at,
                                fmt::format(
                                    "`{}` is `@real_arithmetic` but computes no linear map: it {}",
                                    interner.text( ast.name( function.declaration ) ),
                                    describe( outcome.ending->why )
                                ),
                                "each output must be a sum of inputs scaled by constants"
                            );
                        }
                    }
                    bool shaped = rotate_loops( function, *build.loops );
                    verified( function, "rotate_loops" );
                    // After rotation, so the guard's read and the latch's both reach the body.
                    if( forward_reads( function ) )
                    {
                        verified( function, "forward_reads" );
                        shaped = true;
                    }
                    // At -O3 always: a repeat eliminate_common finds is threaded only by a third round.
                    if( shaped || build.level >= 3 )
                    {
                        optimise_round( function );
                    }
                    // After the round folds the checks that would exit before an invariant.
                    if( hoist_invariants( function, pin_named ) )
                    {
                        verified( function, "hoist_invariants" );
                    }
                    // After hoisting, so `i * n` has an invariant `n`.
                    bool moved = build.level >= 2 && reduce_inductions( function, front.types.table(), literals );
                    if( moved )
                    {
                        verified( function, "reduce_inductions" );
                    }
                    // Last, so the round after folds each copy's index.
                    if( build.level >= 2 &&
                        unroll_loops( function, front.types.table(), literals, *build.loops, build.level >= 3 ) )
                    {
                        verified( function, "unroll_loops" );
                        moved = true;
                    }
                    if( moved )
                    {
                        optimise_round( function );
                    }
                    // Else only the empty blocks normalising added and a hoist went through are left.
                    else
                    {
                        simplify( function, literals );
                        verified( function, "simplify" );
                        merge_blocks( function );
                        verified( function, "merge_blocks" );
                    }
                }
            }

            lower_dispatch( function, ast, front.types.table() );
            verified( function, "lower_dispatch" );
        }

        if( build.level >= 2 && build.inlining )
        {
            for( const u32 f : group )
            {
                finished[f] = true;
            }
        }
        purity.decide( group );
    }
    lap( build.level >= 1 ? "optimise" : "drops" );

    // Last, so no pass inlines the Keel `main` into it.
    if( !as_module )
    {
        if( std::optional<Function> shim =
                main_shim( ast, front.types, literals, interner, front.program.modules, input, front.functions ) )
        {
            verified( *shim, "main_shim" ); // before the push, which moves what the verifier reads
            front.functions.push_back( std::move( *shim ) );
        }
    }

    return front;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/print.h"

namespace keel
{
namespace
{

std::string printed( const Compiled& p )
{
    std::string text;
    for( const Function& function : p.functions )
    {
        text += print( function, p.ast, p.types.table(), p.literals, p.interner );
    }
    return text;
}

} // namespace

TEST_CASE( "front_end_O3_without_the_loop_phase_is_O2", "[front_end]" )
{
    constexpr std::string_view source =
        "i64 sum( i64 n ) { i64 t = 0; for( i64 i = 0; i < n; i += 1 ) { t += i * 3; } return t; }\n"
        "i32 main() { return cast<i32>( sum( 10 ) ); }\n";
    const Compiled o2( source, Stage::Elaborated, Build { .level = 2, .loops = std::nullopt } );
    const Compiled o3( source, Stage::Elaborated, Build { .level = 3, .loops = std::nullopt } );
    REQUIRE( o2.clean() );
    REQUIRE( o3.clean() );
    REQUIRE( printed( o3 ) == printed( o2 ) );
}

} // namespace keel
#endif
