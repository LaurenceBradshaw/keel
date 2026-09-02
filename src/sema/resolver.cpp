#include "sema/resolver.h"
#include <fmt/format.h>
#include <unordered_map>

namespace keel
{
namespace
{

class Resolver
{
public:
    Resolver( const Ast& ast, const Source_manager& sm, const Interner& interner, Diagnostics& diags )
        : ast_( ast ),
          sm_( sm ),
          interner_( interner ),
          diags_( diags )
    {
    }

    Resolution run();

private:
    enum class Scope_kind
    {
        Transparent,
        Barrier
    };

    struct Scope
    {
        std::unordered_map<Symbol_id, Node_id> names;
        Scope_kind                             kind = Scope_kind::Transparent;
    };

    void visit( Node_id id );

    void error_at( Span span, std::string message, std::string help = {} );

    std::string previous_declaration_note( Node_id prev ) const;

    void push_scope( Scope_kind kind = Scope_kind::Transparent );
    void pop_scope();

    void    declare( Symbol_id name, Node_id decl );
    Node_id lookup( Symbol_id name ) const;

    const Ast& ast_;

    const Source_manager& sm_;
    const Interner&       interner_;
    Diagnostics&          diags_;

    std::vector<Node_id> bindings_;
    std::vector<Scope>   scopes_;
};

void Resolver::error_at( Span span, std::string message, std::string help )
{
    diags_.error( span, std::move( message ), std::move( help ) );
}

Resolution Resolver::run()
{
    bindings_.assign( ast_.node_count(), Node_id {} );

    // Two passes at file scope: every top-level declaration is collected before any body is
    // resolved, which is what makes recursion and mutual recursion work with no forward
    // declarations. Inside a function, scopes are populated as the walk proceeds, so using a local
    // before its declaration stays an error.
    push_scope( Scope_kind::Barrier );

    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( decl ) == Node_kind::Function_decl )
        {
            declare( Symbol_id { ast_.aux( decl ) }, decl );
        }
    }

    visit( ast_.root() );
    pop_scope();

    return Resolution( std::move( bindings_ ) );
}

void Resolver::visit( Node_id id )
{
    if( !id.is_valid() ) // auto's missing type, an absent else, empty for clauses
    {
        return;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Error:
        return; // already reported; resolving inside it only cascades
    case Node_kind::Block:
        push_scope();
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        pop_scope();
        return;
    case Node_kind::Function_decl:
        push_scope( Scope_kind::Barrier );
        // Skip return type for the same reason Node_kind::Var_decl skips its type
        visit( ast_.children( id )[1] ); // param list
        visit( ast_.children( id )[2] ); // body
        pop_scope();
        return;
    case Node_kind::Param_decl:
        declare( Symbol_id { ast_.aux( id ) }, id );
        return;
    case Node_kind::For_stmt:
        push_scope();
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        pop_scope();
        return;
    case Node_kind::Var_decl:
        // Initialiser first: `i32 x = x;` must not see its own variable
        visit( ast_.children( id )[1] ); // Var_decl arity of 2: type, initialiser
        declare( Symbol_id { ast_.aux( id ) }, id );
        return;
    case Node_kind::Name_expr:
    {
        const Symbol_id name { ast_.aux( id ) };
        const Node_id   decl = lookup( name );

        if( decl.is_valid() )
        {
            bindings_[id.v] = decl;
        }
        else
        {
            error_at( ast_.span( id ), fmt::format( "`{}` is not declared", interner_.text( name ) ) );
        }

        return;
    }

    default:
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        return;
    }
}

void Resolver::push_scope( Scope_kind kind )
{
    scopes_.emplace_back();
    scopes_.back().kind = kind;
}

void Resolver::pop_scope()
{
    scopes_.pop_back();
}

void Resolver::declare( Symbol_id name, Node_id decl )
{
    assert( !scopes_.empty() );

    if( !name.is_valid() )
    {
        return;
    }

    const auto [it, inserted] = scopes_.back().names.try_emplace( name, decl );

    if( !inserted )
    {
        error_at(
            ast_.span( decl ),
            fmt::format( "`{}` is already declared in this scope", interner_.text( name ) ),
            previous_declaration_note( it->second )
        );
        return;
    }

    // D19. The declaration stays in the map even when it shadows, so later uses bind to the
    // variable actually written rather than to the outer one.
    if( scopes_.back().kind == Scope_kind::Barrier )
    {
        return;
    }

    for( auto s = scopes_.rbegin() + 1; s != scopes_.rend(); ++s )
    {
        const auto found = s->names.find( name );

        if( found != s->names.end() )
        {
            error_at(
                ast_.span( decl ),
                fmt::format( "`{}` shadows an outer declaration", interner_.text( name ) ),
                previous_declaration_note( found->second )
            );
            return;
        }

        if( s->kind == Scope_kind::Barrier )
        {
            return;
        }
    }
}

std::string Resolver::previous_declaration_note( Node_id prev ) const
{
    const Span     span = ast_.span( prev );
    const Line_col loc  = sm_.line_col( span.file, span.start );

    return fmt::format( "previous declaration is at: {}:{}", loc.line, loc.col );
}

Node_id Resolver::lookup( Symbol_id name ) const
{
    if( !name.is_valid() )
    {
        return Node_id {};
    }

    for( auto it = scopes_.rbegin(); it != scopes_.rend(); ++it )
    {
        const auto found = it->names.find( name );
        if( found != it->names.end() )
        {
            return found->second;
        }
    }

    return Node_id {};
}

} // namespace

Resolution resolve( const Ast& ast, const Source_manager& sm, const Interner& interner, Diagnostics& diags )
{
    return Resolver( ast, sm, interner, diags ).run();
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include "common/interner.h"
#include "lex/lexer.h"
#include "parse/parser.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

namespace keel
{
namespace
{

class Resolved
{
public:
    explicit Resolved( std::string_view source )
    {
        file_         = sm_.add_file( "t.kl", std::string( source ) );
        ast_          = parse( lex( file_, sm_, interner_, literals_, diags_ ), sm_, diags_ );
        parse_errors_ = diags_.error_count();
        resolution_   = resolve( ast_, sm_, interner_, diags_ );
    }

    const Ast& ast() const
    {
        return ast_;
    }

    // Errors from resolution only, so a fixture with a deliberate parse error still says something
    // useful about resolution.
    std::size_t errors() const
    {
        return diags_.error_count() - parse_errors_;
    }

    bool clean() const
    {
        return diags_.error_count() == 0;
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags_.render( sm_, out );
        return out.str();
    }

    Node_id declaration_of( Node_id use ) const
    {
        return resolution_.declaration_of( use );
    }

    // The nth node of a kind, in the order the parser created them.
    Node_id nth( Node_kind kind, std::size_t index ) const
    {
        for( u32 i = 0; i < ast_.node_count(); ++i )
        {
            const Node_id id { i };
            if( ast_.kind( id ) == kind && index-- == 0 )
            {
                return id;
            }
        }
        return Node_id {};
    }

    std::string_view text( Node_id id ) const
    {
        return sm_.text( ast_.span( id ) );
    }

private:
    Source_manager sm_;
    Interner       interner_;
    Literals       literals_;
    Diagnostics    diags_;
    File_id        file_;
    Ast            ast_;
    Resolution     resolution_;
    std::size_t    parse_errors_ = 0;
};

} // namespace

// --- step 1: the walk ---

TEST_CASE( "resolver_walks_without_crashing", "[sema][resolve]" )
{
    // Every shape the parser can produce, including the invalid child slots that `auto`, a missing
    // `else` and empty `for` clauses leave behind.
    const Resolved p( "i32 f( i32 a )\n"
                      "{\n"
                      "    auto x = 1;\n"
                      "    if( a ) { }\n"
                      "    for( ; ; ) { }\n"
                      "    return a;\n"
                      "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.ast().root().is_valid() );
}

// Resolving inside a failed subtree only produces cascades.
TEST_CASE( "resolver_skips_error_subtrees", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { @ return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 0 );
}

// --- step 2: local variables ---

TEST_CASE( "resolver_binds_a_local", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { i32 x = 0; return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Node_id use  = p.nth( Node_kind::Name_expr, 0 );
    const Node_id decl = p.nth( Node_kind::Var_decl, 0 );

    REQUIRE( use.is_valid() );
    REQUIRE( p.declaration_of( use ) == decl );
}

// The test that a resolver binding every name to the first declaration it saw would fail.
TEST_CASE( "resolver_distinguishes_sibling_scopes", "[sema][resolve]" )
{
    const Resolved p( "i32 main()\n"
                      "{\n"
                      "    { i32 x = 1; return x; }\n"
                      "    { i32 x = 2; return x; }\n"
                      "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Node_id first  = p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) );
    const Node_id second = p.declaration_of( p.nth( Node_kind::Name_expr, 1 ) );

    REQUIRE( first.is_valid() );
    REQUIRE( second.is_valid() );
    REQUIRE( first != second );
}

TEST_CASE( "resolver_reports_an_unknown_name", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { return nowhere; }" );

    REQUIRE( p.errors() == 1 );
    INFO( p.rendered() );
    REQUIRE( p.rendered().find( "nowhere" ) != std::string::npos );
}

// Statements are sequential inside a function, unlike at file scope.
TEST_CASE( "resolver_rejects_use_before_declaration_in_a_block", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { return x; i32 x = 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

// The initialiser is resolved before the name enters scope, or `i32 x = x;` refers to itself.
TEST_CASE( "resolver_initialiser_cannot_see_its_own_variable", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { i32 x = x; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

TEST_CASE( "resolver_reports_redeclaration_in_one_scope", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { i32 x = 0; i32 x = 1; return x; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

// --- step 3: function scope ---

TEST_CASE( "resolver_binds_a_parameter", "[sema][resolve]" )
{
    const Resolved p( "i32 double_it( i32 value ) { return value; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Node_id use   = p.nth( Node_kind::Name_expr, 0 );
    const Node_id param = p.nth( Node_kind::Param_decl, 0 );

    REQUIRE( p.declaration_of( use ) == param );
}

TEST_CASE( "resolver_parameters_are_not_visible_outside", "[sema][resolve]" )
{
    const Resolved p( "i32 f( i32 a ) { return a; }\ni32 g() { return a; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

// The for variable is visible in the condition, the update and the body, but not after the loop.
TEST_CASE( "resolver_scopes_the_for_variable_to_its_loop", "[sema][resolve]" )
{
    SECTION( "visible throughout the loop" )
    {
        const Resolved p( "i32 main() { for( i32 i = 0; i < 3; i++ ) { i32 y = i; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "not visible after it" )
    {
        const Resolved p( "i32 main() { for( i32 i = 0; i < 3; i++ ) { } return i; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// Every scope-opening construct nests, so a name declared inside one is invisible outside it.
// A transparent scope sees the names around it; the pair to
// resolver_transparent_scopes_do_not_leak_outward, which checks the other direction.
TEST_CASE( "resolver_transparent_scopes_see_outward", "[sema][resolve]" )
{
    static const char* sources[] = {
        "i32 main() { i32 x = 1; { return x; } }",
        "i32 main() { i32 x = 1; if( 1 ) { return x; } return 0; }",
        "i32 main() { i32 x = 1; while( 1 ) { return x; } return 0; }",
        "i32 main() { i32 x = 1; for( ; ; ) { return x; } }",
    };

    for( const char* source : sources )
    {
        const Resolved p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.clean() );

        // Not just resolved - resolved to the outer declaration.
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }
}

TEST_CASE( "resolver_transparent_scopes_do_not_leak_outward", "[sema][resolve]" )
{
    static const char* leaks[] = {
        "i32 main() { if( 1 ) { i32 x = 1; } return x; }",
        "i32 main() { while( 1 ) { i32 x = 1; } return x; }",
        "i32 main() { { i32 x = 1; } return x; }",
        "i32 main() { for( ; ; ) { i32 x = 1; } return x; }",
    };

    for( const char* source : leaks )
    {
        const Resolved p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// D19: a declaration may not hide one that is still reachable by the same unqualified name.
TEST_CASE( "resolver_rejects_shadowing", "[sema][resolve]" )
{
    static const char* shadows[] = {
        "i32 main() { i32 x = 1; { i32 x = 2; return x; } }",
        "i32 main() { i32 x = 1; if( 1 ) { i32 x = 2; return x; } return x; }",
        "i32 f( i32 x ) { i32 x = 1; return x; }",
        "i32 main() { i32 i = 0; for( i32 i = 0; ; ) { } return i; }",
    };

    for( const char* source : shadows )
    {
        const Resolved p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "shadows an outer declaration" ) != std::string::npos );
    }
}

TEST_CASE( "resolver_walks_every_enclosing_transparent_scope", "[sema][resolve]" )
{
    SECTION( "each level collides with the one outside it" )
    {
        const Resolved p( "i32 main() { i32 x = 1; { i32 x = 2; { i32 x = 3; } } }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }

    // The scopes between hold no `x`, so a walk that gave up after one level would miss this.
    SECTION( "the collision is more than one level out" )
    {
        const Resolved p( "i32 main() { i32 x = 1; { { { i32 x = 2; } } } }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Each `x` is out of scope again before the next is declared, so nothing is hidden.
    SECTION( "scopes closed before the outer declaration do not collide" )
    {
        const Resolved p( "i32 main() { { { i32 x = 1; } i32 x = 2; } i32 x = 3; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// File scope is a barrier, so adding a top-level function cannot break a body that already uses
// that name for a local.
TEST_CASE( "resolver_allows_a_local_to_reuse_a_file_scope_name", "[sema][resolve]" )
{
    const Resolved p( "i32 count() { return 1; }\ni32 main() { i32 count = 0; return count; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
    REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
}

TEST_CASE( "resolver_reports_duplicate_declarations", "[sema][resolve]" )
{
    SECTION( "two functions with one name" )
    {
        const Resolved p( "i32 f() { return 1; }\ni32 f() { return 2; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "already declared" ) != std::string::npos );
    }

    SECTION( "two parameters with one name" )
    {
        const Resolved p( "i32 f( i32 a, i32 a ) { return a; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // The message carries the earlier location, since Diagnostics supports only one span per error.
    SECTION( "the message points at the previous declaration" )
    {
        const Resolved p( "i32 main()\n{\n    i32 x = 0;\n    i32 x = 1;\n    return x;\n}\n" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "previous declaration is at: 3:" ) != std::string::npos );
    }
}

// Names appear in more places than just `return x;` - every one goes through the same lookup.
TEST_CASE( "resolver_binds_names_in_every_position", "[sema][resolve]" )
{
    const Resolved p( "i32 g( i32 a ) { return a; }\n"
                      "i32 main()\n"
                      "{\n"
                      "    i32 y = 0;\n"
                      "    y = y + 1;\n"
                      "    y += g( y );\n"
                      "    y++;\n"
                      "    if( y < 3 ) { y = g( y ); }\n"
                      "    while( y < 9 ) { y++; }\n"
                      "    return y;\n"
                      "}\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    // Every Name_expr in the file must have resolved to something.
    std::size_t names = 0;
    for( u32 i = 0; i < p.ast().node_count(); ++i )
    {
        const Node_id id { i };
        if( p.ast().kind( id ) != Node_kind::Name_expr )
        {
            continue;
        }

        ++names;
        INFO( "unresolved name: " << p.text( id ) );
        REQUIRE( p.declaration_of( id ).is_valid() );
    }

    REQUIRE( names > 10 );
}

// --- step 4: file scope ---

TEST_CASE( "resolver_binds_a_call", "[sema][resolve]" )
{
    const Resolved p( "i32 helper() { return 1; }\ni32 main() { return helper(); }\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Node_id call = p.nth( Node_kind::Call_expr, 0 );
    const Node_id use  = p.ast().children( call )[0];

    REQUIRE( p.declaration_of( use ) == p.nth( Node_kind::Function_decl, 0 ) );
}

// The pre-pass over top-level declarations is what makes these work. C++ needs a forward
// declaration for the second; Keel does not.
TEST_CASE( "resolver_allows_recursion", "[sema][resolve]" )
{
    SECTION( "self" )
    {
        const Resolved p( "i32 fib( i32 n ) { return fib( n ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "mutual, with no forward declaration" )
    {
        const Resolved p( "i32 even( i32 n ) { return odd( n ); }\ni32 odd( i32 n ) { return even( n ); }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a function used before it is declared" )
    {
        const Resolved p( "i32 main() { return later(); }\ni32 later() { return 1; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "resolver_reports_an_unknown_call", "[sema][resolve]" )
{
    const Resolved p( "i32 main() { return missing(); }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
