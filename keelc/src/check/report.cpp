// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/report.h"

#include <fmt/format.h>
#include <algorithm>
#include <string>
#include <string_view>
#include "check/assign_check.h"
#include "check/borrow_check.h"
#include "check/move_check.h"

namespace keel
{

namespace
{

// check_moves reports spans and a Local_id; the Interner here turns that local into a name.
// Diagnostics carries one span and a help string rather than a second underlined snippet, so the
// move site becomes a line:col in the help - the shape the resolver's previous-declaration note
// already uses.
void report_move_errors(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    const Source_manager&        sm,
    const Interner&              interner,
    Types&                       types,
    Diagnostics&                 diagnostics
)
{
    for( const Function& function : functions )
    {
        for( const Move_error& error : check_moves( function, owning_locals( function, ast, types ) ) )
        {
            const Symbol_id name = function.locals[error.local.v].name;

            // A temporary can be moved too - the lowerer synthesises one wherever an owning value
            // is handed over - so a move error does not always name something the author wrote.
            const std::string subject =
                name.is_valid() ? fmt::format( "`{}`", interner.text( name ) ) : std::string( "this value" );

            switch( error.conflict )
            {
            case Call_conflict::Moved_and_borrowed:
                diagnostics.error(
                    error.use,
                    fmt::format( "{} is moved into this call, so the call cannot also borrow it", subject ),
                    "a call cannot take a value and borrow it at once"
                );
                continue;
            case Call_conflict::Out_twice:
            {
                const Line_col other = sm.line_col( error.other.file, error.other.start );

                diagnostics.error(
                    error.use,
                    "one place is passed `out` twice to this call",
                    fmt::format(
                        "`{}` at {}:{} overlaps it; give each result its own variable",
                        sm.text( error.other ),
                        other.line,
                        other.col
                    )
                );
                continue;
            }
            case Call_conflict::Aliased:
            {
                const Line_col other = sm.line_col( error.other.file, error.other.start );

                diagnostics.error(
                    error.use,
                    "one object is passed twice to this call, which could change it",
                    fmt::format(
                        "`{}` at {}:{} is the same object; the callee would see it under two names",
                        sm.text( error.other ),
                        other.line,
                        other.col
                    )
                );
                continue;
            }
            case Call_conflict::Element_and_whole:
                diagnostics.error(
                    error.use,
                    fmt::format(
                        "`{}` reaches into {}, which this call is also passed by `{}`",
                        sm.text( error.use ),
                        subject,
                        sm.text( error.other )
                    ),
                    fmt::format( "the call could change {} under it; copy the element out before the call", subject )
                );
                continue;
            case Call_conflict::None:
                break;
            }

            const Line_col at = sm.line_col( error.moved.file, error.moved.start );

            // The two must read differently: `maybe` is the compiler refusing an ambiguity rather
            // than reporting a certainty, and that is the whole reason the state exists.
            diagnostics.error(
                error.use,
                error.maybe ? fmt::format( "{} may already have been moved", subject )
                            : fmt::format( "{} is used after it was moved", subject ),
                error.maybe ? fmt::format( "moved at {}:{} on some path to here", at.line, at.col )
                            : fmt::format( "moved at {}:{}", at.line, at.col )
            );
        }
    }
}

// D54: a loan broken while its holder is still used, named with where the holder was bound.
void report_loan_errors(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    const Source_manager&        sm,
    const Interner&              interner,
    Types&                       types,
    Diagnostics&                 diagnostics
)
{
    for( const Function& function : functions )
    {
        for( const Loan_error& error : check_loans( function, owning_locals( function, ast, types ) ) )
        {
            const Symbol_id   name = function.locals[error.object.v].name;
            const std::string object =
                name.is_valid() ? fmt::format( "`{}`", interner.text( name ) ) : std::string( "this value" );
            const std::string_view holder = interner.text( function.locals[error.holder.v].name );
            const Line_col         taken  = sm.line_col( error.taken.file, error.taken.start );

            std::string message;
            switch( error.conflict )
            {
            case Loan_conflict::Moved:
                message = fmt::format( "{} is moved while `{}` still borrows it", object, holder );
                break;
            case Loan_conflict::Assigned:
                message = fmt::format( "{} is assigned while `{}` still borrows from it", object, holder );
                break;
            case Loan_conflict::Changed:
                message = fmt::format( "this could change {} while `{}` still borrows from it", object, holder );
                break;
            }

            diagnostics.error(
                error.at,
                message,
                fmt::format( "`{}` is used after this; it was bound at {}:{}", holder, taken.line, taken.col )
            );
        }
    }
}

// Two rules out of one analysis. D31: an `out` parameter is one the callee assigns, and "the callee
// must assign it" is a promise to the caller rather than advice - so a path that returns without
// writing one is an error, and the return slot is the same obligation under another name. D9: a
// value read before anything put one there. Same shape as report_move_errors above: the pass
// reports spans and a Local_id, and this names it.
void report_unassigned_errors(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    const Interner&              interner,
    Diagnostics&                 diagnostics,
    const Types&                 types
)
{
    for( const Function& function : functions )
    {
        const Assignment_report report = check_assignment( function );

        const auto field_name = [&]( Node_id field ) { return interner.text( ast.name( field ) ); };

        // D9: a value read before it exists. Reported first, because when a function has both the
        // read is the mistake and the missing assignment at the exit is its consequence.
        for( const Uninitialised_read& read : report.reads )
        {
            if( read.field.is_valid() && read.whole )
            {
                diagnostics.error(
                    read.at,
                    fmt::format( "`this` is used before this constructor assigns `{}`", field_name( read.field ) ),
                    "assign every field before calling a method or using `this`"
                );
                continue;
            }

            if( read.field.is_valid() )
            {
                diagnostics.error(
                    read.at,
                    read.maybe ? fmt::format( "`{}` may be used before this constructor assigns it", field_name( read.field ) )
                               : fmt::format( "`{}` is used before this constructor assigns it", field_name( read.field ) ),
                    read.maybe ? "it is assigned on some paths to here, but not all" : "assign it before reading it"
                );
                continue;
            }

            const Symbol_id name = function.locals[read.local.v].name;

            // A temporary is always written before it is read, so an unnamed local here is a
            // lowering bug rather than the author's - say something rather than nothing.
            const std::string subject =
                name.is_valid() ? fmt::format( "`{}`", interner.text( name ) ) : std::string( "this value" );

            const bool is_out = std::find( function.out_parameters.begin(), function.out_parameters.end(), read.local ) !=
                                function.out_parameters.end();

            // An `out` parameter gets its own wording: the author did write it, so "used before it
            // is initialised" would read as though they had forgotten a declaration.
            if( is_out )
            {
                diagnostics.error(
                    read.at,
                    fmt::format( "{} is read before this function assigns it", subject ),
                    "an `out` parameter holds no value on entry - the caller supplies the storage, not the value"
                );
                continue;
            }

            diagnostics.error(
                read.at,
                read.maybe ? fmt::format( "{} may be used before it is initialised", subject )
                           : fmt::format( "{} is used before it is initialised", subject ),
                read.maybe ? "it is assigned on some paths to here, but not all" : "give it a value at its declaration"
            );
        }

        for( const Unassigned_error& error : report.unassigned )
        {
            if( error.field.is_valid() )
            {
                diagnostics.error(
                    error.at,
                    error.maybe
                        ? fmt::format( "this constructor does not assign `{}` on every path", field_name( error.field ) )
                        : fmt::format( "this constructor never assigns `{}`", field_name( error.field ) ),
                    "every field must hold a value when the constructor returns"
                );
                continue;
            }

            if( error.local == k_return_slot )
            {
                const std::string_view return_type = types.table().name( function.locals[k_return_slot.v].type );

                // Not error.maybe, which cannot answer this one. A path that returns *leaves* the
                // graph, so it never joins the block this is reported at, and `ever` there is
                // always 0 - the flag is meaningful for an `out` parameter, whose paths do join,
                // and structurally false for the return slot. The question the wording wants is
                // about the whole function, so it is asked of the whole function.
                const bool returns_somewhere = std::any_of(
                    function.statements.begin(),
                    function.statements.end(),
                    []( const Statement& statement ) {
                        return statement.kind == Statement_kind::Assign && !statement.place.is_global() &&
                               statement.place.local == k_return_slot;
                    }
                );

                diagnostics.error(
                    error.at,
                    returns_somewhere ? "this function does not return a value on every path"
                                      : "this function never returns a value",
                    fmt::format( "it returns `{}`, so every path out of it must produce one", return_type )
                );
                continue;
            }

            const Symbol_id name = function.locals[error.local.v].name;

            // An `out` parameter always has one, unlike a moved temporary - but reading it from the
            // same place keeps the two reporters saying the same thing about the same field.
            const std::string subject =
                name.is_valid() ? fmt::format( "`{}`", interner.text( name ) ) : std::string( "this parameter" );

            // The two read differently on purpose: one is a path the author missed, the other is a
            // parameter they never wrote to at all. Neither says "this `return`", because the
            // caret is the *function* when the path that misses it is the fall off the end - which
            // is the common case, and the one where naming a return would point at nothing.
            diagnostics.error(
                error.at,
                error.maybe ? fmt::format( "{} is not assigned on every path out of this function", subject )
                            : fmt::format( "{} is never assigned", subject ),
                "an `out` parameter is the callee's promise to assign it"
            );
        }

        for( const Span at : report.diverging_returns )
        {
            diagnostics.error(
                at,
                "this function returns `never`, but can reach its end",
                "end every path in `panic`, a call of another `never` function, or a loop that never exits"
            );
        }

        for( const Reassigned_field& error : report.reassigned )
        {
            diagnostics.error(
                error.at,
                error.maybe ? fmt::format( "`{}` may already hold a value here", field_name( error.field ) )
                            : fmt::format( "`{}` already holds a value here", field_name( error.field ) ),
                error.is_const ? "a constructor assigns a `const` field once on each path"
                               : "a constructor assigns an owning field once on each path"
            );
        }
    }
}

} // namespace

void report_dataflow_errors(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    const Source_manager&        sm,
    const Interner&              interner,
    Types&                       types,
    Diagnostics&                 diagnostics
)
{
    report_move_errors( functions, ast, sm, interner, types, diagnostics );
    report_loan_errors( functions, ast, sm, interner, types, diagnostics );
    report_unassigned_errors( functions, ast, interner, diagnostics, types );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"

namespace keel
{

TEST_CASE( "report_names_the_moved_local", "[check][report]" )
{
    const Compiled p(
        "void sink( i32 x ) { }\n"
        "i32 main() { i32 a = 1; sink( move a ); sink( a ); return 0; }\n",
        Through::report
    );

    REQUIRE( p.rendered().find( "`a` is used after it was moved" ) != std::string::npos );
    REQUIRE( p.rendered().find( "moved at 2:25" ) != std::string::npos );
}

// D54: a refusal within one call names both arguments.
TEST_CASE( "report_names_both_aliased_arguments", "[check][report][m9]" )
{
    const Compiled element(
        "class V { i32 x; V() { x = 0; } ~V() { } public i32* operator[]( u64 i ) const { return nullptr; } };\n"
        "void pair( ref V v, ref i32 e ) { }\n"
        "i32 main() { V v = V(); pair( ref v, ref v[0] ); return 0; }\n",
        Through::report
    );

    INFO( element.rendered() );
    REQUIRE(
        element.rendered().find( "`ref v[0]` reaches into `v`, which this call is also passed by `ref v`" ) != std::string::npos
    );

    const Compiled twice(
        "void outs( out i32 a, out i32 b ) { a = 1; b = 2; }\n"
        "i32 main() { i32 n = 0; outs( out n, out n ); return n; }\n",
        Through::report
    );

    INFO( twice.rendered() );
    REQUIRE( twice.rendered().find( "one place is passed `out` twice to this call" ) != std::string::npos );
    REQUIRE( twice.rendered().find( "`out n` at 2:31 overlaps it" ) != std::string::npos );
}

// D54: a broken loan names the object, the binding still using it, and where that was bound.
TEST_CASE( "report_names_a_broken_loan", "[check][report][m9]" )
{
    const Compiled moved(
        "class B { public u64 n; B() { n = 0; } ~B() { } };\n"
        "void take( move B b ) { }\n"
        "i32 main() { B b = B(); ref B r = b; take( move b ); u64 k = r.n; return 0; }\n",
        Through::report
    );

    INFO( moved.rendered() );
    REQUIRE( moved.rendered().find( "`b` is moved while `r` still borrows it" ) != std::string::npos );
    REQUIRE( moved.rendered().find( "`r` is used after this; it was bound at 3:25" ) != std::string::npos );

    constexpr std::string_view k_vector =
        "class V { public i32 x; V() { x = 0; } ~V() { } public i32* operator[]( u64 i ) const { return nullptr; } "
        "public void push( i32 e ) { } };\n";

    const Compiled changed(
        std::string( k_vector ) + "i32 main() { V v = V(); ref i32 x = v[0]; v.push( 1 ); return x; }\n", Through::report
    );

    INFO( changed.rendered() );
    REQUIRE( changed.rendered().find( "this could change `v` while `x` still borrows from it" ) != std::string::npos );

    const Compiled assigned(
        std::string( k_vector ) + "i32 main() { V v = V(); ref i32 x = v[0]; v = V(); return x; }\n", Through::report
    );

    INFO( assigned.rendered() );
    REQUIRE( assigned.rendered().find( "`v` is assigned while `x` still borrows from it" ) != std::string::npos );
}

TEST_CASE( "report_names_the_unassigned_out_parameter", "[check][report]" )
{
    const Compiled p(
        "void fill( out i32 v ) { }\n"
        "i32 main() { return 0; }\n",
        Through::report
    );

    REQUIRE( p.rendered().find( "`v` is never assigned" ) != std::string::npos );
}

} // namespace keel
#endif
