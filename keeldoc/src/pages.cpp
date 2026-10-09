// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pages.h"
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <set>
#include "html.h"
#include "markdown.h"
#include "primitives.h"
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

bool is_type( std::string_view declares )
{
    return declares == "struct" || declares == "class" || declares == "enum";
}

// The class marking a declaration's name: a constructor or destructor spells its type.
std::string_view name_class( std::string_view declares )
{
    if( declares == "function" || declares == "method" )
    {
        return "fn";
    }
    if( declares == "constructor" || declares == "destructor" || is_type( declares ) )
    {
        return "type";
    }
    return declares == "primitive" ? "prim" : "";
}

bool is_word_char( char c )
{
    return std::isalnum( static_cast<unsigned char>( c ) ) || c == '_';
}

// Whether `text` declares `name` at `at`: the name, then its parameters, after any `<...>`.
bool declares_at( std::string_view text, std::size_t at, std::string_view name )
{
    if( name.empty() || !text.substr( at ).starts_with( name ) || ( at > 0 && is_word_char( text[at - 1] ) ) )
    {
        return false;
    }

    std::size_t i = at + name.size();
    if( i < text.size() && text[i] == '<' )
    {
        for( int depth = 0; i < text.size(); ++i )
        {
            depth += text[i] == '<' ? 1 : text[i] == '>' ? -1 : 0;
            if( depth == 0 )
            {
                ++i;
                break;
            }
        }
    }
    return i < text.size() && text[i] == '(';
}

class Page_writer
{
public:
    explicit Page_writer( const Package& package );

    std::string index() const;
    std::string module( const Module& module ) const;
    std::string prelude() const;

private:
    // A signature with its keywords and the `name` it declares marked, and each of the package's
    // types linked from `page`, except `self`, the type it declares, which is marked instead.
    std::string signature(
        std::string_view text, const std::string& page, std::string_view self, std::string_view declares, std::string_view name
    ) const;

    void
    group( std::string& out, const Group& group, const std::string& id, const std::string& page, std::string_view self ) const;

    // A page's contents list, when it has more than one entry, then each entry and its members.
    void entries( std::string& out, const std::vector<Entry>& entries, const std::string& page ) const;

    std::string head( std::string_view title ) const;

    // The header on every page: the package's summary, or its name when it has no doc, linking
    // to the index.
    std::string        banner() const;
    static std::string foot( const Notice& notice );

    const Package& package_;

    // Each type's page and anchor, by name: the package's, then the prelude's and the primitives.
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

    for( const Entry& entry : package.prelude.entries )
    {
        if( is_type( entry.group.declares() ) || entry.group.declares() == "primitive" )
        {
            types_.try_emplace( entry.group.name, k_prelude_page, entry.group.name );
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

std::string Page_writer::foot( const Notice& notice )
{
    if( notice.copyright.empty() && notice.license.empty() )
    {
        return {};
    }

    std::string text = html_escape( notice.copyright );
    if( !notice.license.empty() )
    {
        text += fmt::format( "{}Licensed under {}.", text.empty() ? "" : ". ", html_escape( notice.license ) );
    }

    return "<footer>" + text + "</footer>\n";
}

std::string Page_writer::signature(
    std::string_view text, const std::string& page, std::string_view self, std::string_view declares, std::string_view name
) const
{
    std::string out;

    for( std::size_t i = 0; i < text.size(); )
    {
        if( !name_class( declares ).empty() && declares_at( text, i, name ) )
        {
            out += fmt::format( "<span class=\"{}\">{}</span>", name_class( declares ), html_escape( name ) );
            i += name.size();
            name = {};
            continue;
        }

        if( !std::isalpha( static_cast<unsigned char>( text[i] ) ) && text[i] != '_' )
        {
            out += html_escape( text.substr( i, 1 ) );
            ++i;
            continue;
        }

        std::size_t end = i;
        while( end < text.size() && is_word_char( text[end] ) )
        {
            ++end;
        }

        const std::string_view word = text.substr( i, end - i );
        i                           = end;

        if( k_keywords.contains( word ) )
        {
            out += fmt::format( "<span class=\"kw\">{}</span>", word );
        }
        else if( const auto type = types_.find( word ); type != types_.end() && word != self )
        {
            const auto& [type_page, anchor] = type->second;
            out += fmt::format(
                "<a class=\"{}\" href=\"{}#{}\">{}</a>",
                is_primitive( word ) ? "prim" : "type",
                type_page == page ? "" : type_page,
                html_escape( anchor ),
                word
            );
        }
        else if( is_primitive( word ) )
        {
            out += fmt::format( "<span class=\"prim\">{}</span>", word );
        }
        else if( word == self )
        {
            out += fmt::format( "<span class=\"type\">{}</span>", word );
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
            signature( decl.signature, page, self, decl.declares, group.name )
        );

        if( !decl.doc.empty() )
        {
            out += "<div class=\"doc\">\n" + render_markdown( decl.doc ) + "</div>\n";
        }
    }

    out += "</div>\n";
}

std::string Page_writer::banner() const
{
    std::string title = render_summary( package_.doc );
    if( title.ends_with( '.' ) )
    {
        title.pop_back();
    }

    return fmt::format(
        "<header><nav><a href=\"index.html\">{}</a></nav></header>\n", title.empty() ? html_escape( package_.name ) : title
    );
}

std::string Page_writer::index() const
{
    std::string out = head( package_.name );

    out += banner();
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

    out += fmt::format(
        "<h2>Prelude</h2>\n<p>Every program also sees the <a href=\"{}\">prelude</a> without an import.</p>\n", k_prelude_page
    );
    out += "</main>\n" + foot( package_.notice ) + "</body>\n</html>\n";
    return out;
}

std::string Page_writer::module( const Module& module ) const
{
    const std::string page  = page_of( module );
    const std::string title = package_.name + "::" + module.name;

    std::string out = head( title );

    out += banner();
    out += "<main>\n";
    out += fmt::format(
        "<h1><span class=\"kind\">module</span> <a class=\"package\" href=\"index.html\">{}</a>::{}</h1>\n",
        html_escape( package_.name ),
        html_escape( module.name )
    );

    entries( out, module.entries, page );

    out += "</main>\n" + foot( package_.notice ) + "</body>\n</html>\n";
    return out;
}

std::string Page_writer::prelude() const
{
    std::string out = head( "prelude" );

    out += banner();
    out += "<main>\n<h1>prelude</h1>\n";

    if( !package_.prelude.doc.empty() )
    {
        out += "<div class=\"doc\">\n" + render_markdown( package_.prelude.doc ) + "</div>\n";
    }

    entries( out, package_.prelude.entries, std::string( k_prelude_page ) );

    out += "</main>\n" + foot( package_.prelude.notice ) + "</body>\n</html>\n";
    return out;
}

void Page_writer::entries( std::string& out, const std::vector<Entry>& entries, const std::string& page ) const
{
    // A contents list only when there is more than one thing to find.
    if( entries.size() > 1 )
    {
        out += "<ul class=\"contents\">\n";
        for( const Entry& entry : entries )
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

    for( const Entry& entry : entries )
    {
        const std::string&     name  = entry.group.name;
        const bool             named = is_type( entry.group.declares() ) || entry.group.declares() == "primitive";
        const std::string_view self  = named ? std::string_view( name ) : std::string_view {};

        out += fmt::format(
            "<section class=\"entry\">\n<h2><span class=\"kind\">{}</span> <span class=\"{}\">{}</span></h2>\n",
            entry.group.declares(),
            name_class( entry.group.declares() ),
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
    pages.emplace( k_prelude_page, writer.prelude() );

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

    REQUIRE( names == std::vector<std::string> { "index.html", "prelude.html", "shapes.html", "style.css", "util.more.html" } );
}

TEST_CASE( "pages_mark_keywords_and_link_types", "[pages]" )
{
    const std::map<std::string, std::string> pages  = render_pages( sample() );
    const std::string&                       shapes = pages.at( "shapes.html" );
    const std::string&                       more   = pages.at( "util.more.html" );

    // The class's own signature does not link to itself; its members do, on the same page.
    REQUIRE(
        shapes.find( "<code><span class=\"kw\">class</span> <span class=\"type\">Box</span></code>" ) != std::string::npos
    );
    REQUIRE(
        shapes.find( "<code><span class=\"kw\">public</span> <span class=\"prim\">void</span> <span class=\"fn\">grow</span>( "
                     "<a class=\"type\" "
                     "href=\"#Box\">Box</a> other )</code>" ) != std::string::npos
    );
    REQUIRE(
        more.find( "shapes::<a class=\"type\" href=\"shapes.html#Box\">Box</a> <span class=\"fn\">make</span>()" ) !=
        std::string::npos
    );
}

TEST_CASE( "pages_head_with_the_package's_summary", "[pages]" )
{
    Package                                  package = sample();
    const std::map<std::string, std::string> pages   = render_pages( package );

    // Every page's header links home under the summary, and a module's title links home by the
    // package's name.
    for( const auto& [name, page] : pages )
    {
        if( name.ends_with( ".html" ) )
        {
            REQUIRE(
                page.find( "<header><nav><a href=\"index.html\">Plane <em>geometry</em></a></nav></header>" ) !=
                std::string::npos
            );
        }
    }
    REQUIRE(
        pages.at( "shapes.html" ).find( "<a class=\"package\" href=\"index.html\">geo</a>::shapes</h1>" ) != std::string::npos
    );

    package.doc = "";
    REQUIRE(
        render_pages( package ).at( "index.html" ).find( "<nav><a href=\"index.html\">geo</a></nav>" ) != std::string::npos
    );
}

TEST_CASE( "pages_mark_the_declared_name", "[pages]" )
{
    const Package package {
        "geo",
        "",
        { Module {
            "wrap",
            { Entry {
                  Group { "Box", { { "class", "Box", "class Box<T>", "" } } },
                  {
                      Section { "Fields", { Group { "cb", { { "field", "cb", "public fn( T ) -> T cb", "" } } } } },
                      Section { "Constructors", { Group { "~Box", { { "destructor", "~Box", "~Box()", "" } } } } },
                      Section {
                          "Methods",
                          { Group { "operator==", { { "method", "operator==", "public bool operator==( Box<T> b )", "" } } } }
                      },
                  },
              },
              Entry { Group { "wrap", { { "function", "wrap", "Box<T> wrap<T>( T v )", "" } } }, {} } },
        } },
        {},
        {},
    };
    const std::string wrap = render_pages( package ).at( "wrap.html" );

    // The name is the one before the parameters, past a return type spelling `<...>` and its own; a
    // constructor or destructor, and a type's own signature and title, mark it as a type.
    REQUIRE( wrap.find( "<span class=\"fn\">wrap</span>&lt;T&gt;( T v )" ) != std::string::npos );
    REQUIRE( wrap.find( "<code><span class=\"type\">~Box</span>()</code>" ) != std::string::npos );
    REQUIRE(
        wrap.find( "<code><span class=\"kw\">class</span> <span class=\"type\">Box</span>&lt;T&gt;</code>" ) !=
        std::string::npos
    );
    REQUIRE( wrap.find( "<h2><span class=\"kind\">class</span> <span class=\"type\">Box</span></h2>" ) != std::string::npos );
    REQUIRE( wrap.find( "<h2><span class=\"kind\">function</span> <span class=\"fn\">wrap</span></h2>" ) != std::string::npos );
    REQUIRE( wrap.find( "<span class=\"fn\">operator==</span>( " ) != std::string::npos );
    REQUIRE( wrap.find( "<span class=\"kw\">fn</span>( T ) -&gt; T cb</code>" ) != std::string::npos );
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

    package.notice         = { "Copyright 2026 A. Author", "Apache-2.0 WITH LLVM-exception" };
    package.prelude.notice = { "Copyright 2025 B", "" };
    for( const auto& [name, page] : render_pages( package ) )
    {
        if( name == "prelude.html" )
        {
            REQUIRE( page.ends_with( "</main>\n<footer>Copyright 2025 B</footer>\n</body>\n</html>\n" ) );
        }
        else if( name.ends_with( ".html" ) )
        {
            REQUIRE( page.ends_with( "</main>\n<footer>Copyright 2026 A. Author. Licensed under Apache-2.0 WITH "
                                     "LLVM-exception.</footer>\n</body>\n</html>\n" ) );
        }
    }
}

TEST_CASE( "pages_link_the_prelude_and_primitives", "[pages]" )
{
    Package package = sample();
    package.prelude = {
        "Seen *everywhere*.",
        {
            Entry { Group { "void", { { "primitive", "void", "void", "No value." } } }, {} },
            Entry { Group { "str", { { "class", "str", "class str", "Text." } } }, {} },
        },
        {},
    };

    const std::map<std::string, std::string> pages   = render_pages( package );
    const std::string&                       shapes  = pages.at( "shapes.html" );
    const std::string&                       prelude = pages.at( "prelude.html" );

    REQUIRE(
        shapes.find( "<a class=\"prim\" href=\"prelude.html#void\">void</a> <span class=\"fn\">grow</span>" ) !=
        std::string::npos
    );
    REQUIRE( shapes.find( "<span class=\"prim\">u64</span>" ) != std::string::npos );

    // A primitive's own entry does not link to itself.
    REQUIRE( prelude.find( "<p>Seen <em>everywhere</em>.</p>" ) != std::string::npos );
    REQUIRE( prelude.find( "<code><span class=\"prim\">void</span></code>" ) != std::string::npos );
    REQUIRE(
        prelude.find( "<h2><span class=\"kind\">primitive</span> <span class=\"prim\">void</span></h2>" ) != std::string::npos
    );
    REQUIRE( pages.at( "index.html" ).find( "<a href=\"prelude.html\">prelude</a>" ) != std::string::npos );
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
