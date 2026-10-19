// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

// The unit-test fixture past type checking. One `Compiled` runs the driver's front end over a
// source string, with the shipped prelude, through the stage a test asks for, so every layer below
// sema tests the pipeline the compiler runs. Included only from inside `#ifdef ENABLE_UNIT_TESTS`.

#include <algorithm>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen_c/halves.h"
#include "front_end.h"

namespace keel
{
namespace
{

struct Compiled
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;
    File_id        file;
    Ast            ast;
    Resolution     resolution;
    Types          types;

    std::vector<Function> all;       // every function lowered, the prelude's included
    std::vector<Function> functions; // the program's C half, in the order lowered

    explicit Compiled( std::string_view source, Stage through = Stage::Simplified, Build build = { .asserts = true } )
    {
        file = sm.add_file( "t.kl", std::string( source ) );

        Front_end front = run_front_end( file, through, build, sm, interner, literals, diags );

        const Halves halves( front.program.ast, front.types.table(), front.program.imports );
        std::ranges::copy_if(
            front.functions,
            std::back_inserter( functions ),
            [&]( const Function& f ) { return halves.of( f ) == C_part::Program; }
        );

        ast        = std::move( front.program.ast );
        resolution = std::move( front.resolution );
        types      = std::move( front.types );
        all        = std::move( front.functions );
    }

    bool clean() const
    {
        return !diags.has_errors();
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags.render( sm, out );
        return out.str();
    }
};

} // namespace
} // namespace keel
