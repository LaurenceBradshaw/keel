#include "common/diagnostics.h"
#include <algorithm>
#include <cstdlib>
#include <ostream>

#include <unistd.h>
#include <string>

namespace keel
{

namespace
{

constexpr std::size_t k_tab_width = 4;

// SGR escapes, or empty strings when colour is off, so the rendering code needs no conditionals.
struct Palette
{
    std::string_view severity;
    std::string_view message;
    std::string_view gutter;
    std::string_view reset;
};

Palette palette_for( Severity severity, bool colour )
{
    if( !colour )
    {
        return {};
    }

    std::string_view tint = "\033[1;31m"; // error: bold red
    if( severity == Severity::Warning )
    {
        tint = "\033[1;33m";
    }
    else if( severity == Severity::Note )
    {
        tint = "\033[1;36m";
    }

    return Palette { tint, "\033[1m", "\033[1;34m", "\033[0m" };
}

const char* severity_label( Severity s )
{
    switch( s )
    {
    case Severity::Error:
        return "error";
    case Severity::Warning:
        return "warning";
    case Severity::Note:
        return "note";
    }
    return "error";
}

// Expands tabs to the next tab stop and records the visual column each byte lands on, so the caret
// can be padded in printed columns rather than bytes. col_of_byte has one extra entry for the
// one-past-the-end position.
std::string expand_tabs( std::string_view line, std::vector<std::size_t>& col_of_byte )
{
    std::string expanded;
    col_of_byte.clear();
    col_of_byte.reserve( line.size() + 1 );

    for( const char c : line )
    {
        col_of_byte.push_back( expanded.size() );
        if( c == '\t' )
        {
            const std::size_t stop = ( expanded.size() / k_tab_width + 1 ) * k_tab_width;
            expanded.append( stop - expanded.size(), ' ' );
        }
        else
        {
            expanded.push_back( c );
        }
    }

    col_of_byte.push_back( expanded.size() );
    return expanded;
}

} // namespace

void Diagnostics::error( Span span, std::string message, std::string help )
{
    items_.push_back( { Severity::Error, span, std::move( message ), std::move( help ) } );
}

void Diagnostics::warning( Span span, std::string message, std::string help )
{
    items_.push_back( { Severity::Warning, span, std::move( message ), std::move( help ) } );
}

bool Diagnostics::has_errors() const
{
    for( const Diagnostic& d : items_ )
    {
        if( d.severity == Severity::Error )
        {
            return true;
        }
    }

    return false;
}

size_t Diagnostics::error_count() const
{
    size_t count = 0;
    for( const Diagnostic& d : items_ )
    {
        if( d.severity == Severity::Error )
        {
            ++count;
        }
    }

    return count;
}

void Diagnostics::render( const Source_manager& sm, std::ostream& out, bool colour ) const
{
    bool first = true;

    // Source order, not emission order. Emission order is pass structure - the lexer speaks before
    // the checker - and carries no meaning for the reader, because absorption already guarantees
    // every diagnostic is an independent mistake rather than a consequence of one above it. If that
    // ever stops being true, this sort goes with it.
    //
    // Indices rather than the items themselves so render() stays const: every caller holds a
    // Diagnostics by const reference. Within one file a byte offset orders identically to
    // (line, column), both being derived from it, so there is nothing to look up.
    std::vector<u32> ordered( items_.size() );

    for( u32 i = 0; i < items_.size(); ++i )
    {
        ordered[i] = i;
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [this]( u32 a, u32 b )
        {
            const Span& x = items_[a].span;
            const Span& y = items_[b].span;

            if( x.file != y.file )
            {
                return x.file < y.file;
            }

            if( x.start != y.start )
            {
                return x.start < y.start;
            }

            // Two at one span keep emission order - there the pass order is the causal one. Said
            // here rather than by stable_sort, whose libstdc++ implementation reaches for a
            // deprecated temporary buffer.
            return a < b;
        }
    );

    for( const u32 index : ordered )
    {
        const Diagnostic& d = items_[index];

        // A blank line between diagnostics: without it the "error:" line butts against the caret
        // line above and it is ambiguous which snippet a message belongs to. Separator rather than
        // terminator, so the output does not end in a blank line.
        if( !first )
        {
            out << "\n";
        }
        first = false;

        const Palette p = palette_for( d.severity, colour );

        out << p.severity << severity_label( d.severity ) << ": " << p.reset << p.message << d.message << p.reset << "\n";

        if( !d.span.is_valid() )
        {
            continue;
        }

        const Source_file& f  = sm.file( d.span.file );
        const Line_col     lc = sm.line_col( d.span.file, d.span.start );

        std::vector<std::size_t> col_of_byte;
        const std::string        line = expand_tabs( sm.line_text( d.span.file, lc.line ), col_of_byte );

        // Byte range of the span within its first line. A span covering a whole function body is
        // clamped to that line: underlining forty lines helps nobody.
        const u32         line_start = f.line_starts[lc.line - 1];
        const std::size_t last_byte  = col_of_byte.size() - 1;
        const std::size_t begin      = std::min( static_cast<std::size_t>( d.span.start - line_start ), last_byte );
        const std::size_t end        = std::clamp( static_cast<std::size_t>( d.span.end - line_start ), begin, last_byte );

        const std::size_t caret_col = col_of_byte[begin];
        // Zero-length spans still get one caret: "expected `;` here" points between two tokens.
        const std::size_t caret_len = std::max<std::size_t>( col_of_byte[end] - caret_col, 1 );

        const std::string number = std::to_string( lc.line );
        const std::string gutter( number.size(), ' ' );

        out << gutter << p.gutter << "--> " << p.reset << f.path << ":" << lc.line << ":" << lc.col << "\n";
        out << gutter << p.gutter << " |" << p.reset << "\n";
        out << p.gutter << number << " | " << p.reset << line << "\n";
        out << gutter << p.gutter << " | " << p.reset << std::string( caret_col, ' ' ) << p.severity
            << std::string( caret_len, '^' );

        if( !d.help.empty() )
        {
            out << " " << d.help;
        }

        out << p.reset;

        out << "\n";
    }
}

bool colour_supported()
{
    return isatty( STDERR_FILENO ) != 0 && std::getenv( "NO_COLOR" ) == nullptr;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <sstream>

namespace keel
{
namespace
{

std::string render_to_string( const Diagnostics& diags, const Source_manager& sm )
{
    std::ostringstream out;
    diags.render( sm, out );
    return out.str();
}

std::vector<std::string> lines_of( const std::string& text )
{
    std::vector<std::string> out;
    std::istringstream       in( text );
    std::string              line;
    while( std::getline( in, line ) )
    {
        out.push_back( line );
    }
    return out;
}

// The caret line is the one whose first '^' is the only non-space content after the gutter.
const std::string* find_caret_line( const std::vector<std::string>& lines )
{
    for( const std::string& l : lines )
    {
        if( l.find( '^' ) != std::string::npos )
        {
            return &l;
        }
    }
    return nullptr;
}

std::size_t caret_count( const std::string& line )
{
    return static_cast<std::size_t>( std::count( line.begin(), line.end(), '^' ) );
}

// Printed column of the first caret, 0-based from the start of the rendered line.
std::size_t caret_indent( const std::string& line )
{
    return line.find( '^' );
}

} // namespace

TEST_CASE( "diagnostics_starts_empty", "[common][diagnostics]" )
{
    Source_manager sm;
    sm.add_file( "a.kl", "i32 x;" );

    const Diagnostics diags;

    REQUIRE_FALSE( diags.has_errors() );
    REQUIRE( diags.error_count() == 0 );
    REQUIRE( render_to_string( diags, sm ).empty() );
}

TEST_CASE( "diagnostics_counts_errors_only", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "i32 x;" );

    Diagnostics diags;
    diags.warning( Span { f, 0, 3 }, "a warning" );

    REQUIRE_FALSE( diags.has_errors() );
    REQUIRE( diags.error_count() == 0 );

    diags.error( Span { f, 0, 3 }, "an error" );
    diags.error( Span { f, 4, 5 }, "another error" );

    REQUIRE( diags.has_errors() );
    REQUIRE( diags.error_count() == 2 );
}

// Pins the exact output format. If you want different spacing or a different gutter, change it here
// and nowhere else - every other test checks properties rather than literal text.
TEST_CASE( "diagnostics_render_exact_format", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "hello.kl", "i32 main()\n{\n    int x = 0;\n}\n" );

    Diagnostics diags;
    diags.error( Span { f, 17, 20 }, "cannot use `int`; Keel has fixed-width primitives only", "use `i32` instead" );

    const std::string expected = "error: cannot use `int`; Keel has fixed-width primitives only\n"
                                 " --> hello.kl:3:5\n"
                                 "  |\n"
                                 "3 |     int x = 0;\n"
                                 "  |     ^^^ use `i32` instead\n";

    REQUIRE( render_to_string( diags, sm ) == expected );
}

TEST_CASE( "diagnostics_severity_labels", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "i32 x;" );

    Diagnostics diags;
    diags.error( Span { f, 0, 3 }, "the error" );
    const std::string err = render_to_string( diags, sm );
    REQUIRE( err.find( "error: the error" ) != std::string::npos );

    Diagnostics warns;
    warns.warning( Span { f, 0, 3 }, "the warning" );
    const std::string warn = render_to_string( warns, sm );
    REQUIRE( warn.find( "warning: the warning" ) != std::string::npos );
    REQUIRE( warn.find( "error:" ) == std::string::npos );
}

TEST_CASE( "diagnostics_caret_matches_span", "[common][diagnostics]" )
{
    Source_manager sm;
    //                                        0123456789
    const File_id f = sm.add_file( "a.kl", "i32 add( i32 a );" );

    Diagnostics diags;
    diags.error( Span { f, 4, 7 }, "about add" ); // "add", col 5

    const auto         lines = lines_of( render_to_string( diags, sm ) );
    const std::string* caret = find_caret_line( lines );

    REQUIRE( caret != nullptr );
    REQUIRE( caret_count( *caret ) == 3 );

    // The caret must sit under "add" in the rendered source line.
    const std::string& source_line = lines[lines.size() - 2];
    REQUIRE( caret_indent( *caret ) == source_line.find( "add" ) );
}

TEST_CASE( "diagnostics_empty_span_renders_one_caret", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "i32 x" );

    Diagnostics diags;
    diags.error( Span::point( f, 5 ), "expected `;`" ); // between the last token and EOF

    const auto         lines = lines_of( render_to_string( diags, sm ) );
    const std::string* caret = find_caret_line( lines );

    REQUIRE( caret != nullptr );
    REQUIRE( caret_count( *caret ) == 1 );
}

TEST_CASE( "diagnostics_span_at_eof", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "i32 main()\n{\n" );

    Diagnostics diags;
    diags.error( Span::point( f, 13 ), "unexpected end of input" ); // offset == text.size()

    const std::string out = render_to_string( diags, sm );

    REQUIRE( out.find( "unexpected end of input" ) != std::string::npos );
    REQUIRE( find_caret_line( lines_of( out ) ) != nullptr );
}

// A function's span covers its whole body; underlining forty lines is useless. Show the first line
// only, and never let the caret run past the end of that line.
TEST_CASE( "diagnostics_multiline_span_clamps_to_first_line", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "i32 main()\n{\n    return 0;\n}\n" );

    Diagnostics diags;
    diags.error( Span { f, 0, 27 }, "about the whole function" );

    const auto         lines = lines_of( render_to_string( diags, sm ) );
    const std::string* caret = find_caret_line( lines );

    REQUIRE( caret != nullptr );
    REQUIRE( lines.size() == 5 ); // header, location, gutter, one source line, caret line

    const std::string& source_line = lines[3];
    REQUIRE( source_line.find( "i32 main()" ) != std::string::npos );
    REQUIRE( source_line.find( "return" ) == std::string::npos );
    REQUIRE( caret_indent( *caret ) + caret_count( *caret ) <= source_line.size() );
}

TEST_CASE( "diagnostics_help_omitted_when_absent", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "i32 x;" );

    Diagnostics diags;
    diags.error( Span { f, 0, 3 }, "no help here" );

    const auto         lines = lines_of( render_to_string( diags, sm ) );
    const std::string* caret = find_caret_line( lines );

    REQUIRE( caret != nullptr );
    // Nothing after the carets, and no trailing whitespace.
    REQUIRE( caret->back() == '^' );
}

// Emission order is pass structure - the lexer speaks before the checker - and carries no meaning
// for the reader, because absorption already guarantees each diagnostic is an independent mistake
// rather than a consequence of one above it. So they render in source order instead.
TEST_CASE( "diagnostics_render_in_source_order", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "i32 x;\ni32 y;\n" );

    SECTION( "a later span reported first still renders second" )
    {
        Diagnostics diags;
        diags.error( Span { f, 11, 12 }, "further down" );
        diags.error( Span { f, 4, 5 }, "higher up" );

        const std::string out = render_to_string( diags, sm );

        REQUIRE( out.find( "higher up" ) < out.find( "further down" ) );
    }

    // The one place emission order is causal: at an identical span, the pass that ran first is
    // the one that found the cause.
    SECTION( "two at one span keep emission order" )
    {
        Diagnostics diags;
        diags.error( Span { f, 4, 5 }, "reported first" );
        diags.error( Span { f, 4, 5 }, "reported second" );

        const std::string out = render_to_string( diags, sm );

        REQUIRE( out.find( "reported first" ) < out.find( "reported second" ) );
    }

    // render() is const and orders a local index vector, so it must not depend on having been
    // called before - which is the whole reason it does not sort items_ in place.
    SECTION( "rendering twice gives the same output" )
    {
        Diagnostics diags;
        diags.error( Span { f, 11, 12 }, "further down" );
        diags.error( Span { f, 4, 5 }, "higher up" );

        REQUIRE( render_to_string( diags, sm ) == render_to_string( diags, sm ) );
    }
}

TEST_CASE( "diagnostics_render_names_the_right_file", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  a = sm.add_file( "alpha.kl", "i32 x;" );
    const File_id  b = sm.add_file( "beta.kl", "i32 y;" );

    Diagnostics diags;
    diags.error( Span { a, 0, 3 }, "in alpha" );
    diags.error( Span { b, 0, 3 }, "in beta" );

    const std::string out = render_to_string( diags, sm );

    REQUIRE( out.find( "alpha.kl:1:1" ) != std::string::npos );
    REQUIRE( out.find( "beta.kl:1:1" ) != std::string::npos );
}

TEST_CASE( "diagnostics_last_line_without_trailing_newline", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "i32 x;\ni32 yy;" );

    Diagnostics diags;
    diags.error( Span { f, 11, 13 }, "about yy" );

    const auto         lines = lines_of( render_to_string( diags, sm ) );
    const std::string* caret = find_caret_line( lines );

    REQUIRE( caret != nullptr );
    REQUIRE( caret_count( *caret ) == 2 );
    REQUIRE( lines[3].find( "i32 yy;" ) != std::string::npos );
}

// Columns count bytes, but rendering is visual: tabs must be expanded in the source line AND in the
// caret padding, or the caret drifts.
TEST_CASE( "diagnostics_expands_tabs", "[common][diagnostics]" )
{
    Source_manager sm;
    const File_id  f = sm.add_file( "a.kl", "\tint x;" );

    Diagnostics diags;
    diags.error( Span { f, 1, 4 }, "about int" ); // "int", one byte after the tab

    const auto         lines = lines_of( render_to_string( diags, sm ) );
    const std::string* caret = find_caret_line( lines );

    REQUIRE( caret != nullptr );

    const std::string& source_line = lines[3];
    REQUIRE( source_line.find( '\t' ) == std::string::npos ); // expanded
    REQUIRE( caret->find( '\t' ) == std::string::npos );
    REQUIRE( caret_count( *caret ) == 3 );
    REQUIRE( caret_indent( *caret ) == source_line.find( "int" ) ); // aligned, whatever the tab width
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
