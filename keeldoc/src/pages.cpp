// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pages.h"
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <set>
#include "html.h"
#include "markdown.h"
#include "style.h"

namespace keeldoc
{
namespace
{

const std::set<std::string_view> k_keywords = {
    "class",
    "const",
    "enum",
    "extern",
    "fn",
    "move",
    "operator",
    "out",
    "private",
    "public",
    "ref",
    "static",
    "struct",
    "unsafe",
    "where",
};

const std::set<std::string_view> k_primitives = {
    "bool",
    "f32",
    "f64",
    "i8",
    "i16",
    "i32",
    "i64",
    "u8",
    "u16",
    "u32",
    "u64",
    "void",
};

bool is_type( std::string_view declares )
{
    return declares == "struct" || declares == "class" || declares == "enum";
}

class Page_writer
{
public:
    explicit Page_writer( const Package& package );

    std::string index() const;
    std::string module( const Module& module ) const;

private:
    // A signature with its keywords marked and each of the package's types linked from `page`,
    // except `self`, the type it declares.
    std::string signature( std::string_view text, const std::string& page, std::string_view self ) const;

    void
    group( std::string& out, const Group& group, const std::string& id, const std::string& page, std::string_view self ) const;

    std::string head( std::string_view title ) const;
    std::string foot() const;

    const Package& package_;

    // Each type's page and anchor, by name.
    std::map<std::string, std::pair<std::string, std::string>, std::less<>> types_;
};

Page_writer::Page_writer( const Package& package )
    : package_( package )
{
    for( const Module& module : package.modules )
    {
        for( const Entry& entry : module.entries )
        {
            if( is_type( entry.group.declares() ) )
            {
                types_.try_emplace( entry.group.name, page_of( module ), entry.group.name );
            }
        }
    }
}

std::string Page_writer::head( std::string_view title ) const
{
    return fmt::format(
        "<!doctype html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>{}</title>\n"
        "<link rel=\"stylesheet\" href=\"style.css\">\n"
        "</head>\n"
        "<body>\n",
        html_escape( title )
    );
}

std::string Page_writer::foot() const
{
    if( package_.copyright.empty() && package_.license.empty() )
    {
        return {};
    }

    std::string notice = html_escape( package_.copyright );
    if( !package_.license.empty() )
    {
        notice += fmt::format( "{}Licensed under {}.", notice.empty() ? "" : ". ", html_escape( package_.license ) );
    }

    return "<footer>" + notice + "</footer>\n";
}

std::string Page_writer::signature( std::string_view text, const std::string& page, std::string_view self ) const
{
    std::string out;

    for( std::size_t i = 0; i < text.size(); )
    {
        if( !std::isalpha( static_cast<unsigned char>( text[i] ) ) && text[i] != '_' )
        {
            out += html_escape( text.substr( i, 1 ) );
            ++i;
            continue;
        }

        std::size_t end = i;
        while( end < text.size() && ( std::isalnum( static_cast<unsigned char>( text[end] ) ) || text[end] == '_' ) )
        {
            ++end;
        }

        const std::string_view word = text.substr( i, end - i );
        i                           = end;

        if( k_keywords.contains( word ) )
        {
            out += fmt::format( "<span class=\"kw\">{}</span>", word );
        }
        else if( k_primitives.contains( word ) )
        {
            out += fmt::format( "<span class=\"prim\">{}</span>", word );
        }
        else if( const auto type = types_.find( word ); type != types_.end() && word != self )
        {
            const auto& [type_page, anchor] = type->second;
            out += fmt::format(
                "<a class=\"type\" href=\"{}#{}\">{}</a>", type_page == page ? "" : type_page, html_escape( anchor ), word
            );
        }
        else
        {
            out += html_escape( word );
        }
    }

    return out;
}

void Page_writer::group(
    std::string& out, const Group& group, const std::string& id, const std::string& page, std::string_view self
) const
{
    out += fmt::format( "<div class=\"group\" id=\"{}\">\n", html_escape( id ) );

    for( const Declaration& decl : group.overloads )
    {
        out += fmt::format(
            "<pre class=\"signature\"><a class=\"anchor\" href=\"#{}\" aria-label=\"Link to {}\">#</a><code>{}</code></pre>\n",
            html_escape( id ),
            html_escape( group.name ),
            signature( decl.signature, page, self )
        );

        if( !decl.doc.empty() )
        {
            out += "<div class=\"doc\">\n" + render_markdown( decl.doc ) + "</div>\n";
        }
    }

    out += "</div>\n";
}

std::string Page_writer::index() const
{
    std::string out = head( package_.name );

    out += fmt::format( "<header><nav><span class=\"here\">{}</span></nav></header>\n", html_escape( package_.name ) );
    out += "<main>\n";
    out += fmt::format( "<h1><span class=\"kind\">package</span> {}</h1>\n", html_escape( package_.name ) );

    if( !package_.doc.empty() )
    {
        out += "<div class=\"doc\">\n" + render_markdown( package_.doc ) + "</div>\n";
    }

    out += "<h2>Modules</h2>\n";

    for( const Module& module : package_.modules )
    {
        const std::string page = page_of( module );

        out += fmt::format(
            "<section class=\"module\">\n<h3><a href=\"{}\">{}</a></h3>\n<dl class=\"summaries\">\n",
            page,
            html_escape( module.name )
        );

        for( const Entry& entry : module.entries )
        {
            const Group& group = entry.group;
            out += fmt::format(
                "<dt><a href=\"{}#{}\"><code>{}</code></a> <span class=\"kind\">{}</span></dt>\n<dd>{}</dd>\n",
                page,
                html_escape( group.name ),
                html_escape( group.name ),
                group.declares(),
                render_summary( group.overloads.front().doc )
            );
        }

        out += "</dl>\n</section>\n";
    }

    out += "</main>\n" + foot() + "</body>\n</html>\n";
    return out;
}

std::string Page_writer::module( const Module& module ) const
{
    const std::string page  = page_of( module );
    const std::string title = package_.name + "::" + module.name;

    std::string out = head( title );

    out += fmt::format(
        "<header><nav><a href=\"index.html\">{}</a><span class=\"sep\">::</span><span "
        "class=\"here\">{}</span></nav></header>\n",
        html_escape( package_.name ),
        html_escape( module.name )
    );
    out += "<main>\n";
    out += fmt::format( "<h1><span class=\"kind\">module</span> {}</h1>\n", html_escape( title ) );

    // A contents list only when there is more than one thing to find.
    if( module.entries.size() > 1 )
    {
        out += "<ul class=\"contents\">\n";
        for( const Entry& entry : module.entries )
        {
            out += fmt::format(
                "<li><a href=\"#{}\"><code>{}</code></a> <span class=\"kind\">{}</span></li>\n",
                html_escape( entry.group.name ),
                html_escape( entry.group.name ),
                entry.group.declares()
            );
        }
        out += "</ul>\n";
    }

    for( const Entry& entry : module.entries )
    {
        const std::string&     name = entry.group.name;
        const std::string_view self = is_type( entry.group.declares() ) ? std::string_view( name ) : std::string_view {};

        out += fmt::format(
            "<section class=\"entry\">\n<h2><span class=\"kind\">{}</span> {}</h2>\n",
            entry.group.declares(),
            html_escape( name )
        );
        group( out, entry.group, name, page, self );

        for( const Section& section : entry.sections )
        {
            out += fmt::format( "<h3>{}</h3>\n", section.title );

            for( const Group& member : section.groups )
            {
                // A constructor or destructor spells its type's name, so it does not link it.
                const bool spells_type = member.declares() == "constructor" || member.declares() == "destructor";
                group(
                    out, member, name + "." + member.name, page, spells_type ? std::string_view( name ) : std::string_view {}
                );
            }
        }

        out += "</section>\n";
    }

    out += "</main>\n" + foot() + "</body>\n</html>\n";
    return out;
}

} // namespace

std::string page_of( const Module& module )
{
    std::string page = module.name;
    for( std::size_t at = page.find( "::" ); at != std::string::npos; at = page.find( "::" ) )
    {
        page.replace( at, 2, "." );
    }

    return page + ".html";
}

std::map<std::string, std::string> render_pages( const Package& package )
{
    const Page_writer writer( package );

    std::map<std::string, std::string> pages;
    pages.emplace( "index.html", writer.index() );
    pages.emplace( "style.css", std::string( k_style ) );

    for( const Module& module : package.modules )
    {
        pages.emplace( page_of( module ), writer.module( module ) );
    }

    return pages;
}

} // namespace keeldoc

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keeldoc
{
namespace
{

Package sample()
{
    return Package {
        "geo",
        "Plane *geometry*.",
        {
            Module {
                "shapes",
                {
                    Entry {
                        Group { "Box", { { "class", "Box", "class Box", "A box.\n\nWith more." } } },
                        { Section {
                            "Methods",
                            { Group {
                                "grow",
                                {
                                    { "method", "grow", "public void grow( Box other )", "" },
                                    { "method", "grow", "public void grow( u64 by )", "### Panics\n\nOn overflow." },
                                },
                            } },
                        } },
                    },
                },
            },
            Module { "util::more", { Entry { Group { "make", { { "function", "make", "shapes::Box make()", "" } } }, {} } } },
        },
        {},
        {},
    };
}

TEST_CASE( "pages_name_one_file_per_module", "[pages]" )
{
    const std::map<std::string, std::string> pages = render_pages( sample() );

    std::vector<std::string> names;
    for( const auto& [name, _] : pages )
    {
        names.push_back( name );
    }

    REQUIRE( names == std::vector<std::string> { "index.html", "shapes.html", "style.css", "util.more.html" } );
}

TEST_CASE( "pages_mark_keywords_and_link_types", "[pages]" )
{
    const std::map<std::string, std::string> pages  = render_pages( sample() );
    const std::string&                       shapes = pages.at( "shapes.html" );
    const std::string&                       more   = pages.at( "util.more.html" );

    // The class's own signature does not link to itself; its members do, on the same page.
    REQUIRE( shapes.find( "<code><span class=\"kw\">class</span> Box</code>" ) != std::string::npos );
    REQUIRE(
        shapes.find( "<code><span class=\"kw\">public</span> <span class=\"prim\">void</span> grow( <a class=\"type\" "
                     "href=\"#Box\">Box</a> other )</code>" ) != std::string::npos
    );
    REQUIRE( more.find( "shapes::<a class=\"type\" href=\"shapes.html#Box\">Box</a> make()" ) != std::string::npos );
}

TEST_CASE( "pages_keep_overloads_under_one_anchor", "[pages]" )
{
    const std::string shapes = render_pages( sample() ).at( "shapes.html" );

    REQUIRE( shapes.find( "<div class=\"group\" id=\"Box.grow\">" ) != std::string::npos );
    REQUIRE( shapes.find( "id=\"Box.grow\"", shapes.find( "id=\"Box.grow\"" ) + 1 ) == std::string::npos );
    REQUIRE( shapes.find( "<h4>Panics</h4>" ) != std::string::npos );
}

TEST_CASE( "pages_end_with_the_package's_notice", "[pages]" )
{
    Package package = sample();
    REQUIRE( render_pages( package ).at( "index.html" ).find( "<footer>" ) == std::string::npos );

    package.copyright = "Copyright 2026 A. Author";
    package.license   = "Apache-2.0 WITH LLVM-exception";
    for( const auto& [name, page] : render_pages( package ) )
    {
        if( name.ends_with( ".html" ) )
        {
            REQUIRE( page.ends_with( "</main>\n<footer>Copyright 2026 A. Author. Licensed under Apache-2.0 WITH "
                                     "LLVM-exception.</footer>\n</body>\n</html>\n" ) );
        }
    }
}

TEST_CASE( "pages_index_lists_each_module_with_summaries", "[pages]" )
{
    const std::string index = render_pages( sample() ).at( "index.html" );

    REQUIRE( index.find( "<p>Plane <em>geometry</em>.</p>" ) != std::string::npos );
    REQUIRE(
        index.find(
            "<dt><a href=\"shapes.html#Box\"><code>Box</code></a> <span class=\"kind\">class</span></dt>\n<dd>A box.</dd>"
        ) != std::string::npos
    );
    REQUIRE( index.find( "<h3><a href=\"util.more.html\">util::more</a></h3>" ) != std::string::npos );
}

} // namespace
} // namespace keeldoc
#endif // ENABLE_UNIT_TESTS
