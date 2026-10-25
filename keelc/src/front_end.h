// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <vector>
#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "ir/kir.h"
#include "opt/inline_calls.h"
#include "parse/loader.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{

// How far the front end runs past checking, in order. Each stage runs only if nothing before it
// reported an error.
enum class Stage : u8
{
    Lowered,    // as lowered, before anything rewrites it
    Simplified, // what every analysis reads
    Reported,   // the dataflow checks, worded
    Elaborated, // drops made explicit, as the emitter reads them
};

// What a build adds at run time. The diagnostics are the same for every build.
struct Build
{
    bool asserts         = false;
    bool overflow_checks = false;
    bool optimise        = false; // M11's optimisations; release only, so a debug build is the program as lowered.

    // How an optimised build inlines; none leaves every call a call.
    std::optional<Inline_weights> inlining = Inline_weights {};

    // Scalar replacement; a test of an earlier pass turns it off to see that pass's fields.
    bool splitting = true;

    // How often the passes after drop elaboration run; a test of what one round keeps sets 1.
    u32 rounds = 2;

    // Keep each function as the dataflow checks left it, which a module's object holds.
    bool keep_checked = false;
};

struct Front_end
{
    Program               program;
    Resolution            resolution;
    Types                 types;
    std::vector<Function> functions; // every function lowered, the prelude's included
    std::vector<Function> checked;   // under keep_checked: the same, before elaboration
};

// The driver and the test fixture both run this, so the tests run the compiler's own pipeline.
Front_end run_front_end(
    File_id                  input,
    Stage                    through,
    Build                    build,
    Source_manager&          sm,
    Interner&                interner,
    Literal_pool&            literals,
    Diagnostics&             diags,
    std::span<const Package> packages  = {},
    bool                     as_module = false
);

} // namespace keel
