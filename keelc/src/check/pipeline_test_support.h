// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

// The unit-test fixture past type checking. One `Compiled` runs the driver's front end over a
// source string, with the shipped prelude, through the stage a test asks for, so every layer below
// sema tests the pipeline the compiler runs. Included only from inside `#ifdef ENABLE_UNIT_TESTS`.

#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "check/drop_flags.h"
#include "check/report.h"
#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "ir/lower.h"
#include "ir/simplify.h"
#include "parse/loader.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{
namespace
{

// How far a `Compiled` runs, in the driver's order. Each stage runs only if nothing before it
// reported an error.
enum class Through
{
    lower,     // as lowered, before anything rewrites it
    simplify,  // what every analysis reads
    report,    // the dataflow checks, worded
    elaborate, // drops made explicit, as the emitter reads them
};

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
    std::vector<Function> functions; // the input file's, in the order lowered

    explicit Compiled( std::string_view source, Through through = Through::simplify )
    {
        file = sm.add_file( "t.kl", std::string( source ) );

        Program program = load_program( file, sm, interner, literals, diags );
        ast             = std::move( program.ast );
        resolution      = resolve( ast, sm, interner, diags, program.imports );
        types           = type_check( ast, resolution, literals, sm, interner, diags );

        if( diags.has_errors() )
        {
            return;
        }

        all = lower( ast, resolution, types, literals );

        if( through != Through::lower )
        {
            for( Function& function : all )
            {
                simplify( function, literals );
            }
        }

        if( through == Through::report || through == Through::elaborate )
        {
            report_dataflow_errors( all, ast, sm, interner, types, diags );
        }

        if( through == Through::elaborate && !diags.has_errors() )
        {
            const Flag_vocabulary vocabulary {
                .bool_type     = types.table().builtin( Type_kind::Bool ),
                .false_literal = literals.add_integer( 0 ),
                .true_literal  = literals.add_integer( 1 )
            };

            for( Function& function : all )
            {
                elaborate_drops( function, vocabulary );
            }
        }

        for( const Function& function : all )
        {
            if( ast.span( function.declaration ).file == file )
            {
                functions.push_back( function );
            }
        }
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
