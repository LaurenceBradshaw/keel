#include "ast/dump.h"

#include "common/dump_util.h"

#include <fmt/format.h>

#include <ostream>

namespace keel
{
namespace
{

// Indented kind, then location, then the source text for leaves. Deeply nested trees overflow the
// kind column rather than wrapping - alignment degrades, nothing breaks.
constexpr std::size_t k_kind_width     = 40;
constexpr std::size_t k_location_width = 12;
constexpr std::size_t k_indent_step    = 2;

void dump_node( const Ast& ast, const Source_manager& sm, std::ostream& out, Node_id id, std::size_t depth )
{
    const Span     span  = ast.span( id );
    const Line_col start = sm.line_col( span.file, span.start );
    const Line_col end   = sm.line_col( span.file, span.end );

    const std::string label    = std::string( depth * k_indent_step, ' ' ) + std::string( node_kind_name( ast.kind( id ) ) );
    const std::string location = fmt::format( "{}:{}-{}:{}", start.line, start.col, end.line, end.col );

    const auto children = ast.children( id );

    // Only leaves show their text: a Function_decl's span covers its whole body, which would print
    // the entire function on one line.
    if( children.empty() )
    {
        out << fmt::format(
            "{:<{}}{:<{}}\"{}\"\n", label, k_kind_width, location, k_location_width, escape_for_dump( sm.text( span ) )
        );
    }
    else
    {
        out << fmt::format( "{:<{}}{}\n", label, k_kind_width, location );
    }

    // children() points into the Ast's storage and is only valid until the next add(); nothing
    // mutates during a dump, so holding it across the loop is safe here.
    for( const Node_id child : children )
    {
        // An absent optional - the else of an if without one - keeps its slot so arity stays fixed.
        if( !child.is_valid() )
        {
            out << fmt::format( "{:<{}}-\n", std::string( ( depth + 1 ) * k_indent_step, ' ' ) + "<none>", k_kind_width );
            continue;
        }

        dump_node( ast, sm, out, child, depth + 1 );
    }
}

} // namespace

void dump_ast( const Ast& ast, const Source_manager& sm, std::ostream& out )
{
    if( !ast.root().is_valid() )
    {
        return;
    }

    dump_node( ast, sm, out, ast.root(), 0 );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <sstream>

namespace keel
{
namespace
{

// Builds the tree for the source it is given, by hand - the parser does not exist yet.
struct Fixture
{
    Source_manager sm;
    Ast            ast;
    File_id        file;

    explicit Fixture( std::string_view source )
    {
        file = sm.add_file( "t.kl", std::string( source ) );
    }

    Span at( u32 start, u32 end ) const
    {
        return Span { file, start, end };
    }

    std::string dump() const
    {
        std::ostringstream out;
        dump_ast( ast, sm, out );
        return out.str();
    }
};

} // namespace

TEST_CASE( "dump_ast_prints_nothing_without_a_root", "[ast][dump]" )
{
    Fixture f( "i32 x;" );

    REQUIRE( f.dump().empty() );

    f.ast.add( Node_kind::Int_literal, f.at( 0, 1 ), 0, {} ); // a node, but still no root
    REQUIRE( f.dump().empty() );
}

TEST_CASE( "dump_ast_prints_a_leaf", "[ast][dump]" )
{
    Fixture f( "i32" );

    f.ast.set_root( f.ast.add( Node_kind::Named_type, f.at( 0, 3 ), 0, {} ) );

    REQUIRE( f.dump() == "Named_type                              1:1-1:4     \"i32\"\n" );
}

// Pins the exact format. Change it here and nowhere else - every other test checks properties.
TEST_CASE( "dump_ast_exact_format", "[ast][dump]" )
{
    Fixture f( "i32 main()\n{\n    return 0;\n}\n" );

    const Node_id ret_type = f.ast.add( Node_kind::Named_type, f.at( 0, 3 ), 0, {} );
    const Node_id params   = f.ast.add( Node_kind::Param_list, f.at( 8, 10 ), 0, {} );
    const Node_id zero     = f.ast.add( Node_kind::Int_literal, f.at( 24, 25 ), 0, {} );
    const Node_id ret      = f.ast.add( Node_kind::Return_stmt, f.at( 17, 26 ), 0, { zero } );
    const Node_id body     = f.ast.add( Node_kind::Block, f.at( 11, 28 ), 0, { ret } );
    const Node_id func     = f.ast.add( Node_kind::Function_decl, f.at( 0, 28 ), 0, { ret_type, params, body } );

    f.ast.set_root( f.ast.add( Node_kind::Source_file, f.at( 0, 29 ), 0, { func } ) );

    const std::string expected = "Source_file                             1:1-5:1\n"
                                 "  Function_decl                         1:1-4:2\n"
                                 "    Named_type                          1:1-1:4     \"i32\"\n"
                                 "    Param_list                          1:9-1:11    \"()\"\n"
                                 "    Block                               2:1-4:2\n"
                                 "      Return_stmt                       3:5-3:14\n"
                                 "        Int_literal                     3:12-3:13   \"0\"\n";

    REQUIRE( f.dump() == expected );
}

TEST_CASE( "dump_ast_indents_by_depth", "[ast][dump]" )
{
    Fixture f( "0" );

    const Node_id leaf   = f.ast.add( Node_kind::Int_literal, f.at( 0, 1 ), 0, {} );
    const Node_id inner  = f.ast.add( Node_kind::Block, f.at( 0, 1 ), 0, { leaf } );
    const Node_id middle = f.ast.add( Node_kind::Block, f.at( 0, 1 ), 0, { inner } );

    f.ast.set_root( f.ast.add( Node_kind::Block, f.at( 0, 1 ), 0, { middle } ) );

    const std::string out = f.dump();

    REQUIRE( out.find( "\nBlock" ) == std::string::npos );   // root at column 0
    REQUIRE( out.find( "\n  Block" ) != std::string::npos ); // depth 1
    REQUIRE( out.find( "\n    Block" ) != std::string::npos );
    REQUIRE( out.find( "\n      Int_literal" ) != std::string::npos );
}

// Only leaves show text: a Function_decl's span covers the whole function.
TEST_CASE( "dump_ast_shows_text_for_leaves_only", "[ast][dump]" )
{
    Fixture f( "i32 main()" );

    const Node_id type = f.ast.add( Node_kind::Named_type, f.at( 0, 3 ), 0, {} );
    f.ast.set_root( f.ast.add( Node_kind::Function_decl, f.at( 0, 10 ), 0, { type } ) );

    const std::string out = f.dump();

    REQUIRE( out.find( "\"i32\"" ) != std::string::npos );
    REQUIRE( out.find( "\"i32 main()\"" ) == std::string::npos );
}

TEST_CASE( "dump_ast_escapes_literal_text", "[ast][dump]" )
{
    Fixture f( "\"a\\\"b\"" );

    f.ast.set_root( f.ast.add( Node_kind::Int_literal, f.at( 0, 6 ), 0, {} ) );

    REQUIRE( f.dump().find( "\"\\\"a\\\\\\\"b\\\"\"" ) != std::string::npos );
}

// The absent else of an if keeps its slot, so arity stays fixed - the dump has to show that.
TEST_CASE( "dump_ast_marks_absent_optional_children", "[ast][dump]" )
{
    Fixture f( "0" );

    const Node_id cond = f.ast.add( Node_kind::Int_literal, f.at( 0, 1 ), 0, {} );
    f.ast.set_root( f.ast.add( Node_kind::Return_stmt, f.at( 0, 1 ), 0, { cond, Node_id {} } ) );

    const std::string out = f.dump();

    REQUIRE( out.find( "<none>" ) != std::string::npos );
    REQUIRE( out.find( "Int_literal" ) != std::string::npos );
}

TEST_CASE( "dump_ast_handles_deep_nesting_without_wrapping", "[ast][dump]" )
{
    Fixture f( "0" );

    Node_id current = f.ast.add( Node_kind::Int_literal, f.at( 0, 1 ), 0, {} );
    for( int i = 0; i < 30; ++i )
    {
        current = f.ast.add( Node_kind::Block, f.at( 0, 1 ), 0, { current } );
    }
    f.ast.set_root( current );

    const std::string out = f.dump();

    // 31 nodes, one line each, and the deepest overflows the kind column rather than wrapping.
    REQUIRE( static_cast<std::size_t>( std::count( out.begin(), out.end(), '\n' ) ) == 31 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
