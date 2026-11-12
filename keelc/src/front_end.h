// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <algorithm>
#include <chrono>
#include <span>
#include <string_view>
#include <vector>
#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "ir/kir.h"
#include "ir/loops.h"
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
    u32  level           = 0; // -O<n>: 0 is the program as lowered; which passes run at each is in §3.3.

    // How an `-O2` build inlines; none leaves every call a call.
    std::optional<Inline_weights> inlining = Inline_weights {};

    // How an `-O3` build's loop phase runs; none skips it.
    std::optional<Loop_weights> loops = Loop_weights {};

    // Scalar replacement; a test of an earlier pass turns it off to see that pass's fields.
    bool splitting = true;

    // `ref` forwarding; a test of an addressed local turns it off to keep the address.
    bool forwarding = true;

    // How often the passes after drop elaboration run; a test of what one round keeps sets 1.
    u32 rounds = 2;

    // Coalescing again after the rounds; a test of what the first coalescing keeps turns it off.
    bool recoalescing = true;

    // Keep each function as the dataflow checks left it, which a module's object holds.
    bool keep_checked = false;
};

// Times each phase of a run; under --verbose, prints each as it ends.
class Phase_clock
{
public:
    explicit Phase_clock( bool print )
        : print_( print )
    {
    }

    void   lap( std::string_view phase );
    double seconds() const; // since the clock was made

private:
    using Clock = std::chrono::steady_clock;

    bool              print_;
    Clock::time_point start_ = Clock::now();
    Clock::time_point last_  = start_;
};

struct Front_end
{
    Program               program;
    Resolution            resolution;
    Types                 types;
    std::vector<Function> functions; // every function lowered, the prelude's included
    std::vector<File_id>  provided;  // the module each of the first functions was loaded from
    std::vector<File_id>  reused;    // the loaded parts linked from their objects' code: debug, at this level
    std::vector<Function> checked;   // under keep_checked: the same, before elaboration

    bool reuses( File_id part ) const
    {
        return std::find( reused.begin(), reused.end(), part ) != reused.end();
    }
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
    std::span<const Package> packages     = {},
    bool                     as_module    = false,
    std::span<const Object>  objects      = {},
    std::string_view         prelude      = prelude_source(),
    std::string_view         prelude_name = prelude_path,
    Phase_clock*             clock        = nullptr
);

} // namespace keel
