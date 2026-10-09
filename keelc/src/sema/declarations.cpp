// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/declarations.h"
#include <algorithm>
#include <map>
#include "common/json.h"
#include "sema/signature_printer.h"

namespace keel
{
namespace
{

class Declaration_collector
{
public:
    Declaration_collector(
        const Ast& ast, const Resolution& resolution, const Types& types, const Source_manager& sm, const Interner& interner
    )
        : ast_( ast ),
          resolution_( resolution ),
          types_( types ),
          sm_( sm ),
          interner_( interner )
    {
    }

    std::vector<Declaration> run();

private:
    void add( Node_id decl, std::string_view kind, Node_id parent );

    const Ast&            ast_;
    const Resolution&     resolution_;
    const Types&          types_;
    const Source_manager& sm_;
    const Interner&       interner_;

    std::vector<Declaration> declarations_;
};

std::vector<Declaration> Declaration_collector::run()
{
    if( !ast_.root().is_valid() )
    {
        return {};
    }

    for( const Node_id decl : ast_.declarations( ast_.root() ) )
    {
        switch( ast_.kind( decl ) )
        {
        case Node_kind::Function_decl:
            add( decl, "function", Node_id {} );
            break;
        case Node_kind::Var_decl:
            add( decl, "global", Node_id {} );
            break;
        case Node_kind::Enum_decl:
            add( decl, "enum", Node_id {} );

            for( const Node_id variant : ast_.variants( decl ) )
            {
                add( variant, "variant", decl );
            }
            break;
        case Node_kind::Struct_decl:
        case Node_kind::Class_decl:
            add( decl, ast_.kind( decl ) == Node_kind::Struct_decl ? "struct" : "class", Node_id {} );

            for( const Node_id member : ast_.members( decl ) )
            {
                switch( ast_.kind( member ) )
                {
                case Node_kind::Field_decl:
                    add( member, "field", decl );
                    break;
                case Node_kind::Var_decl:
                    add( member, "static_field", decl );
                    break;
                case Node_kind::Method_decl:
                    add( member, "method", decl );
                    break;
                case Node_kind::Constructor_decl:
                    add( member, "constructor", decl );
                    break;
                case Node_kind::Destructor_decl:
                    add( member, "destructor", decl );
                    break;
                default:
                    break;
                }
            }
            break;
        default:
            break;
        }
    }

    std::ranges::stable_sort(
        declarations_,
        []( const Declaration& a, const Declaration& b )
        { return std::pair( a.span.file.v, a.span.start ) < std::pair( b.span.file.v, b.span.start ); }
    );

    return std::move( declarations_ );
}

void Declaration_collector::add( Node_id decl, std::string_view kind, Node_id parent )
{
    // A declaration the parser gave up on has no name to show.
    if( ast_.broken( decl ) || !ast_.name( decl ).is_valid() )
    {
        return;
    }

    declarations_.push_back(
        { ast_.name_span( decl ),
          kind,
          ast_.access( decl ),
          print_signature( ast_, resolution_, types_, interner_, decl ),
          parent.is_valid() ? std::string( interner_.text( ast_.name( parent ) ) ) : std::string {},
          documentation( ast_, sm_, decl ) }
    );
}

} // namespace

std::vector<Declaration> collect_declarations(
    const Ast& ast, const Resolution& resolution, const Types& types, const Source_manager& sm, const Interner& interner
)
{
    return Declaration_collector( ast, resolution, types, sm, interner ).run();
}

void render_declarations_json(
    const Source_manager&                       sm,
    const Interner&                             interner,
    const std::unordered_map<u32, std::string>& package_docs,
    const std::vector<Declaration>&             declarations,
    std::ostream&                               out
)
{
    // By name, so the output does not depend on the map's order; the program's own package is "".
    std::map<std::string_view, std::string_view> packages;
    for( const auto& [symbol, doc] : package_docs )
    {
        if( !doc.empty() )
        {
            packages.emplace( symbol == Symbol_id {}.v ? std::string_view {} : interner.text( Symbol_id { symbol } ), doc );
        }
    }

    for( const auto& [name, doc] : packages )
    {
        out << R"({"kind":"package","name":")" << json_escape( name ) << R"(","doc":")" << json_escape( doc ) << "\"}\n";
    }

    for( const Declaration& decl : declarations )
    {
        const Line_col start = sm.line_col( decl.span.file, decl.span.start );
        const Line_col end   = sm.line_col( decl.span.file, decl.span.end );

        out << R"({"kind":"declaration","declares":")" << decl.kind << R"(","name":")" << json_escape( sm.text( decl.span ) )
            << R"(","file":")" << json_escape( sm.file( decl.span.file ).path ) << R"(","line":)" << start.line << R"(,"col":)"
            << start.col << R"(,"end_col":)" << end.col << R"(,"access":")"
            << ( decl.access == Access::Private ? "private" : "public" ) << R"(","signature":")"
            << json_escape( decl.signature ) << '"';

        if( !decl.parent.empty() )
        {
            out << R"(,"parent":")" << json_escape( decl.parent ) << '"';
        }

        if( !decl.doc.empty() )
        {
            out << R"(,"doc":")" << json_escape( decl.doc ) << '"';
        }

        out << "}\n";
    }
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include "common/literal_pool.h"
#include "lex/lexer.h"
#include "parse/parser.h"

namespace keel
{
namespace
{

TEST_CASE( "declarations_list_members_and_variants_with_their_parent", "[sema][declarations]" )
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;

    const File_id file = sm.add_file(
        "t.kl",
        "/// A colour.\n"
        "enum Colour { Red, /// Not red.\n Green };\n"
        "class Box\n"
        "{\n"
        "    /// Makes one.\n"
        "    public Box( i32 v ) { n = v; }\n"
        "    public i32 get() const { return n; }\n"
        "    private i32 n;\n"
        "};\n"
        "i32 counter = 0;\n"
        "void f() {}\n"
    );
    const Ast        ast        = parse( lex( file, sm, interner, literals, diags ), sm, diags );
    const Resolution resolution = resolve( ast, sm, interner, diags );
    const Types      types      = type_check( ast, resolution, literals, sm, interner, diags );

    REQUIRE_FALSE( diags.has_errors() );

    std::vector<std::string> listed;
    for( const Declaration& decl : collect_declarations( ast, resolution, types, sm, interner ) )
    {
        listed.push_back( fmt::format( "{} {} [{}] {}", decl.kind, decl.signature, decl.parent, decl.doc ) );
    }

    REQUIRE(
        listed ==
        std::vector<std::string> {
            "enum enum Colour [] A colour.",
            "variant Red [Colour] ",
            "variant Green [Colour] Not red.",
            "class class Box [] ",
            "constructor public Box( i32 v ) [Box] Makes one.",
            "method public i32 get() const [Box] ",
            "field private i32 n [Box] ",
            "global i32 counter [] ",
            "function void f() [] ",
        }
    );
}

TEST_CASE( "declarations_render_as_json_lines_after_their_packages", "[sema][declarations]" )
{
    Source_manager  sm;
    Interner        interner;
    const File_id   file = sm.add_file( "a.kl", "void get() {}" );
    const Symbol_id kl   = interner.intern( "kl" );

    std::ostringstream out;
    render_declarations_json(
        sm,
        interner,
        { { kl.v, "The library." }, { Symbol_id {}.v, "" } },
        { { Span { file, 5, 8 }, "method", Access::Private, "private void get()", "Box", "Gets \"it\"." } },
        out
    );

    REQUIRE(
        out.str() == "{\"kind\":\"package\",\"name\":\"kl\",\"doc\":\"The library.\"}\n"
                     "{\"kind\":\"declaration\",\"declares\":\"method\",\"name\":\"get\",\"file\":\"a.kl\",\"line\":1,\"col\":"
                     "6,\"end_col\":9,"
                     "\"access\":\"private\",\"signature\":\"private void get()\",\"parent\":\"Box\","
                     "\"doc\":\"Gets \\\"it\\\".\"}\n"
    );
}

} // namespace
} // namespace keel
#endif // ENABLE_UNIT_TESTS
