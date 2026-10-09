// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "markdown.h"
#include <cctype>
#include <optional>
#include <vector>
#include "html.h"

namespace keeldoc
{
namespace
{

enum class List_kind
{
    None,
    Bullet,
    Ordered,
};

std::vector<std::string_view> split_lines( std::string_view text )
{
    std::vector<std::string_view> lines;
    std::size_t                   start = 0;

    while( start <= text.size() )
    {
        const std::size_t end = text.find( '\n', start );
        if( end == std::string_view::npos )
        {
            lines.push_back( text.substr( start ) );
            break;
        }

        lines.push_back( text.substr( start, end - start ) );
        start = end + 1;
    }

    return lines;
}

std::string_view trim( std::string_view text )
{
    while( !text.empty() && std::isspace( static_cast<unsigned char>( text.front() ) ) )
    {
        text.remove_prefix( 1 );
    }

    while( !text.empty() && std::isspace( static_cast<unsigned char>( text.back() ) ) )
    {
        text.remove_suffix( 1 );
    }

    return text;
}

bool is_blank( std::string_view line )
{
    return trim( line ).empty();
}

bool is_fence( std::string_view line )
{
    return trim( line ).starts_with( "```" );
}

// The heading's text, or nothing when `line` is not one.
std::optional<std::string_view> heading( std::string_view line )
{
    line = trim( line );

    std::size_t level = 0;
    while( level < line.size() && line[level] == '#' )
    {
        ++level;
    }

    if( level == 0 || level > 6 || ( level < line.size() && line[level] != ' ' ) )
    {
        return std::nullopt;
    }

    std::string_view rest = trim( line.substr( level ) );
    while( !rest.empty() && rest.back() == '#' )
    {
        rest.remove_suffix( 1 );
    }

    return trim( rest );
}

// What kind of list item `line` starts, and its text after the marker.
std::pair<List_kind, std::string_view> list_item( std::string_view line )
{
    line = trim( line );

    if( line.size() >= 2 && ( line[0] == '-' || line[0] == '*' || line[0] == '+' ) && line[1] == ' ' )
    {
        return { List_kind::Bullet, trim( line.substr( 2 ) ) };
    }

    std::size_t digits = 0;
    while( digits < line.size() && std::isdigit( static_cast<unsigned char>( line[digits] ) ) )
    {
        ++digits;
    }

    if( digits > 0 && digits + 1 < line.size() && ( line[digits] == '.' || line[digits] == ')' ) && line[digits + 1] == ' ' )
    {
        return { List_kind::Ordered, trim( line.substr( digits + 2 ) ) };
    }

    return { List_kind::None, {} };
}

// Whether `line` starts a block of its own, so it ends a paragraph or a list item.
bool starts_block( std::string_view line )
{
    return is_blank( line ) || is_fence( line ) || heading( line ) || list_item( line ).first != List_kind::None;
}

// The run of backticks at `at`, and where its closing run starts; npos when none closes it.
std::pair<std::size_t, std::size_t> code_span( std::string_view text, std::size_t at )
{
    std::size_t run = 0;
    while( at + run < text.size() && text[at + run] == '`' )
    {
        ++run;
    }

    for( std::size_t i = at + run; i < text.size(); )
    {
        if( text[i] != '`' )
        {
            ++i;
            continue;
        }

        std::size_t close = 0;
        while( i + close < text.size() && text[i + close] == '`' )
        {
            ++close;
        }

        if( close == run )
        {
            return { run, i };
        }

        i += close;
    }

    return { run, std::string_view::npos };
}

// The next `delimiter` after `from` outside code spans and escapes, accepted by `fits`.
template <class Fits>
std::size_t find_closing( std::string_view text, std::size_t from, std::string_view delimiter, Fits fits )
{
    for( std::size_t i = from; i < text.size(); )
    {
        if( text[i] == '\\' )
        {
            i += 2;
            continue;
        }

        if( text[i] == '`' )
        {
            const auto [run, close] = code_span( text, i );
            i                       = close == std::string_view::npos ? i + run : close + run;
            continue;
        }

        if( text.substr( i ).starts_with( delimiter ) && fits( i ) )
        {
            return i;
        }

        ++i;
    }

    return std::string_view::npos;
}

} // namespace

std::string render_inline( std::string_view text )
{
    std::string out;

    for( std::size_t i = 0; i < text.size(); )
    {
        const char c = text[i];

        if( c == '\\' && i + 1 < text.size() && std::ispunct( static_cast<unsigned char>( text[i + 1] ) ) )
        {
            out += html_escape( text.substr( i + 1, 1 ) );
            i += 2;
            continue;
        }

        if( c == '`' )
        {
            const auto [run, close] = code_span( text, i );
            if( close == std::string_view::npos )
            {
                out.append( run, '`' );
                i += run;
                continue;
            }

            std::string_view code = text.substr( i + run, close - i - run );
            if( code.size() >= 2 && code.front() == ' ' && code.back() == ' ' )
            {
                code = code.substr( 1, code.size() - 2 );
            }

            out += "<code>" + html_escape( code ) + "</code>";
            i = close + run;
            continue;
        }

        if( text.substr( i ).starts_with( "**" ) )
        {
            const std::size_t close = find_closing( text, i + 3, "**", []( std::size_t ) { return true; } );
            if( close != std::string_view::npos && text[i + 2] != ' ' )
            {
                out += "<strong>" + render_inline( text.substr( i + 2, close - i - 2 ) ) + "</strong>";
                i = close + 2;
                continue;
            }
        }
        else if( c == '*' && i + 1 < text.size() && text[i + 1] != ' ' )
        {
            const std::size_t close = find_closing( text, i + 2, "*", [&]( std::size_t at ) { return text[at - 1] != ' '; } );
            if( close != std::string_view::npos )
            {
                out += "<em>" + render_inline( text.substr( i + 1, close - i - 1 ) ) + "</em>";
                i = close + 1;
                continue;
            }
        }

        if( c == '[' )
        {
            const std::size_t label_end = find_closing( text, i + 1, "](", []( std::size_t ) { return true; } );
            const std::size_t url_end   = label_end == std::string_view::npos ? label_end : text.find( ')', label_end + 2 );
            if( url_end != std::string_view::npos )
            {
                const std::string_view url = trim( text.substr( label_end + 2, url_end - label_end - 2 ) );
                out += "<a href=\"" + html_escape( url ) + "\">" + render_inline( text.substr( i + 1, label_end - i - 1 ) ) +
                       "</a>";
                i = url_end + 1;
                continue;
            }
        }

        out += html_escape( text.substr( i, 1 ) );
        ++i;
    }

    return out;
}

std::string render_markdown( std::string_view text )
{
    const std::vector<std::string_view> lines = split_lines( text );

    std::string out;
    std::size_t i = 0;

    const auto joined = [&]( std::size_t from, std::size_t to )
    {
        std::string text;
        for( std::size_t at = from; at < to; ++at )
        {
            text += ( at == from ? "" : "\n" );
            text += trim( lines[at] );
        }

        return text;
    };

    while( i < lines.size() )
    {
        const std::string_view line = lines[i];

        if( is_blank( line ) )
        {
            ++i;
            continue;
        }

        if( is_fence( line ) )
        {
            const std::size_t start = ++i;
            while( i < lines.size() && !is_fence( lines[i] ) )
            {
                ++i;
            }

            std::string code;
            for( std::size_t at = start; at < i; ++at )
            {
                code += std::string( lines[at] ) + "\n";
            }

            out += "<pre><code>" + html_escape( code ) + "</code></pre>\n";
            ++i; // the closing fence, or the end
            continue;
        }

        if( const std::optional<std::string_view> title = heading( line ) )
        {
            out += "<h4>" + render_inline( *title ) + "</h4>\n";
            ++i;
            continue;
        }

        if( const List_kind kind = list_item( line ).first; kind != List_kind::None )
        {
            out += kind == List_kind::Bullet ? "<ul>\n" : "<ol>\n";

            while( i < lines.size() && list_item( lines[i] ).first == kind )
            {
                std::string item( list_item( lines[i] ).second );
                for( ++i; i < lines.size() && !starts_block( lines[i] ); ++i )
                {
                    item += "\n" + std::string( trim( lines[i] ) );
                }

                out += "<li>" + render_inline( item ) + "</li>\n";

                // A blank line between two items of one list keeps it one list.
                std::size_t next = i;
                while( next < lines.size() && is_blank( lines[next] ) )
                {
                    ++next;
                }

                if( next < lines.size() && list_item( lines[next] ).first == kind )
                {
                    i = next;
                }
            }

            out += kind == List_kind::Bullet ? "</ul>\n" : "</ol>\n";
            continue;
        }

        const std::size_t start = i;
        for( ++i; i < lines.size() && !starts_block( lines[i] ); ++i )
        {
        }

        out += "<p>" + render_inline( joined( start, i ) ) + "</p>\n";
    }

    return out;
}

std::string render_summary( std::string_view text )
{
    const std::vector<std::string_view> lines = split_lines( text );

    std::string summary;
    for( const std::string_view line : lines )
    {
        if( is_blank( line ) )
        {
            if( !summary.empty() )
            {
                break;
            }

            continue;
        }

        summary += ( summary.empty() ? "" : "\n" );
        summary += trim( line );
    }

    return render_inline( summary );
}

} // namespace keeldoc

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keeldoc
{
namespace
{

TEST_CASE( "markdown_inline_code_emphasis_and_links", "[markdown]" )
{
    REQUIRE(
        render_inline( "Takes `list<T>` and **moves** it." ) ==
        "Takes <code>list&lt;T&gt;</code> and <strong>moves</strong> it."
    );
    REQUIRE( render_inline( "an *odd* one" ) == "an <em>odd</em> one" );
    REQUIRE( render_inline( "see [the plan](docs/PLAN.md#d53)" ) == "see <a href=\"docs/PLAN.md#d53\">the plan</a>" );
    REQUIRE( render_inline( "``a ` b``" ) == "<code>a ` b</code>" );
    REQUIRE( render_inline( "`` `x` ``" ) == "<code>`x`</code>" );
}

TEST_CASE( "markdown_inline_leaves_what_does_not_close", "[markdown]" )
{
    REQUIRE( render_inline( "a `stray tick" ) == "a `stray tick" );
    REQUIRE( render_inline( "2 * 3 * 4" ) == "2 * 3 * 4" );
    REQUIRE( render_inline( "*not `closed*` here" ) == "*not <code>closed*</code> here" );
    REQUIRE( render_inline( "\\*literal\\*" ) == "*literal*" );
    REQUIRE( render_inline( "[no link]" ) == "[no link]" );
    REQUIRE( render_inline( "a < b && c" ) == "a &lt; b &amp;&amp; c" );
}

TEST_CASE( "markdown_blocks", "[markdown]" )
{
    REQUIRE(
        render_markdown( "The summary,\nover two lines.\n\n### Panics\n\nIf empty.\n" ) ==
        "<p>The summary,\nover two lines.</p>\n<h4>Panics</h4>\n<p>If empty.</p>\n"
    );
    REQUIRE(
        render_markdown( "Some:\n- `a`: one\n  more\n- b\n\n- c\n\n1. first\n2. second" ) ==
        "<p>Some:</p>\n<ul>\n<li><code>a</code>: one\nmore</li>\n<li>b</li>\n<li>c</li>\n</ul>\n"
        "<ol>\n<li>first</li>\n<li>second</li>\n</ol>\n"
    );
    REQUIRE(
        render_markdown( "Use:\n```\nkl::list<i32> v;\n  v.push( 1 );\n```\nDone." ) ==
        "<p>Use:</p>\n<pre><code>kl::list&lt;i32&gt; v;\n  v.push( 1 );\n</code></pre>\n<p>Done.</p>\n"
    );
    REQUIRE( render_markdown( "#hashtag is text" ) == "<p>#hashtag is text</p>\n" );
    REQUIRE( render_markdown( "" ).empty() );
}

TEST_CASE( "markdown_summary_is_the_first_paragraph", "[markdown]" )
{
    REQUIRE( render_summary( "\nFirst `one`,\ncontinued.\n\nSecond." ) == "First <code>one</code>,\ncontinued." );
    REQUIRE( render_summary( "" ).empty() );
}

} // namespace
} // namespace keeldoc
#endif // ENABLE_UNIT_TESTS
