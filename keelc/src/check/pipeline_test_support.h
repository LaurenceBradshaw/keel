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

#include "codegen/parts.h"
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
    Module_graph   modules;
    Resolution     resolution;
    Types          types;

    std::vector<Function> all;       // every function lowered, the prelude's included
    std::vector<Function> functions; // every part's but the prelude's and the entry, in the order lowered

    explicit Compiled( std::string_view source, Stage through = Stage::Simplified, Build build = { .asserts = true } )
    {
        file = sm.add_file( "t.kl", std::string( source ) );

        Front_end front = run_front_end( file, through, build, sm, interner, literals, diags );

        const Parts parts( front.program.ast, front.types, front.program.modules, file, front.functions );
        std::ranges::copy_if(
            front.functions,
            std::back_inserter( functions ),
            [&]( const Function& f ) { return !f.entry && parts.of( f ) != front.program.modules.prelude_file(); }
        );

        ast        = std::move( front.program.ast );
        modules    = std::move( front.program.modules );
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

    std::vector<std::string> shown() const
    {
        return shown( diags, sm );
    }

    // Each diagnostic render() shows, as `line:col-line:col severity: message [-Wname] = help`.
    static std::vector<std::string> shown( const Diagnostics& diags, const Source_manager& sm )
    {
        std::vector<std::string> lines;
        for( const Diagnostic& d : diags.shown() )
        {
            std::string line;
            if( d.span.is_valid() )
            {
                const Line_col start = sm.line_col( d.span.file, d.span.start );
                const Line_col end   = sm.line_col( d.span.file, d.span.end );
                line = std::to_string( start.line ) + ":" + std::to_string( start.col ) + "-" + std::to_string( end.line ) +
                       ":" + std::to_string( end.col ) + " ";
            }
            line += d.severity == Severity::Error ? "error: " : d.severity == Severity::Warning ? "warning: " : "note: ";
            line += d.message;
            if( d.warning )
            {
                line += " [-W" + std::string( warning_name( *d.warning ) ) + "]";
            }
            if( !d.help.empty() )
            {
                line += " = " + d.help;
            }
            lines.push_back( std::move( line ) );
        }
        return lines;
    }
};

} // namespace
} // namespace keel
