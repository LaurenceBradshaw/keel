#include "sema/reporter.h"

#include <fmt/format.h>

namespace keel::sema
{

void Reporter::error_at( Span span, std::string message, std::string help )
{
    diags_.error( span, std::move( message ), std::move( help ) );
}

void Reporter::warn_at( Span span, std::string message, std::string help )
{
    diags_.warning( span, std::move( message ), std::move( help ) );
}

std::string Reporter::previous_declaration_note( Span previous ) const
{
    const Line_col loc = sm_.line_col( previous.file, previous.start );

    return fmt::format( "previous declaration is at: {}:{}", loc.line, loc.col );
}

std::string_view Reporter::text( Span span ) const
{
    return sm_.text( span );
}

std::size_t Reporter::error_count() const
{
    return diags_.error_count();
}

} // namespace keel::sema

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{
namespace
{

TEST_CASE( "reporter_forwards_errors_and_warnings_and_counts_only_errors", "[sema][reporter]" )
{
    Source_manager sm;
    Diagnostics    diags;
    const File_id  file = sm.add_file( "t.kl", "let x = 1;\nlet y = 2;\n" );

    sema::Reporter reporter( sm, diags );

    REQUIRE( reporter.error_count() == 0 );

    reporter.warn_at( Span { file, 0, 3 }, "a warning" );
    REQUIRE( reporter.error_count() == 0 );

    reporter.error_at( Span { file, 0, 3 }, "an error", "some help" );
    REQUIRE( reporter.error_count() == 1 );
}

TEST_CASE( "reporter_names_the_line_and_column_of_an_earlier_declaration", "[sema][reporter]" )
{
    Source_manager sm;
    Diagnostics    diags;
    const File_id  file = sm.add_file( "t.kl", "let x = 1;\nlet y = 2;\n" );

    sema::Reporter reporter( sm, diags );

    // Second line, first column - the note must say so rather than repeat the byte offset.
    REQUIRE( reporter.previous_declaration_note( Span { file, 11, 14 } ) == "previous declaration is at: 2:1" );
}

TEST_CASE( "reporter_quotes_the_source_back", "[sema][reporter]" )
{
    Source_manager sm;
    Diagnostics    diags;
    const File_id  file = sm.add_file( "t.kl", "let x = 1;\n" );

    sema::Reporter reporter( sm, diags );

    REQUIRE( reporter.text( Span { file, 4, 5 } ) == "x" );
}

} // namespace
} // namespace keel
#endif
