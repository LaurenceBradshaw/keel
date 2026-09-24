#include "sema/resolver.h"
#include <fmt/format.h>
#include <unordered_map>
#include <unordered_set>

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
    bool    chain_overload( Node_id existing, Node_id added );
    Node_id lookup( Symbol_id name ) const;

    const Ast& ast_;

    const Source_manager& sm_;
    const Interner&       interner_;
    Diagnostics&          diags_;

    std::vector<Node_id> bindings_;
    std::vector<Node_id> next_overload_;
    std::vector<Scope>   scopes_;

    // The enclosing aggregate's field names, for D19's member clause. Empty outside a member body,
    // so the check it drives costs one lookup per declaration everywhere else.
    std::unordered_set<Symbol_id> current_fields_;
};

void Resolver::error_at( Span span, std::string message, std::string help )
{
    diags_.error( span, std::move( message ), std::move( help ) );
}

Resolution Resolver::run()
{
    bindings_.assign( ast_.node_count(), Node_id {} );
    next_overload_.assign( ast_.node_count(), Node_id {} );

    // Two passes at file scope: every top-level declaration is collected before any body is
    // resolved, which is what makes recursion and mutual recursion work with no forward
    // declarations. Inside a function, scopes are populated as the walk proceeds, so using a local
    // before its declaration stays an error.
    push_scope( Scope_kind::Barrier );

    for( const Node_id decl : ast_.children( ast_.root() ) )
    {
        if( ast_.kind( decl ) == Node_kind::Function_decl || ast_.kind( decl ) == Node_kind::Var_decl ||
            ast_.kind( decl ) == Node_kind::Enum_decl || is_aggregate( ast_.kind( decl ) ) )
        {
            declare( Symbol_id { ast_.aux( decl ) }, decl );
        }
    }

    visit( ast_.root() );
    pop_scope();

    return Resolution( std::move( bindings_ ), std::move( next_overload_ ) );
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
    case Node_kind::Source_file:
        for( const Node_id decl : ast_.children( id ) )
        {
            // Declared already by the pre-pass. Going through the Var_decl case would declare it a
            // second time and report it against itself. Functions and structs do not hit this
            // because their cases do not declare - only Var_decl does.
            if( ast_.kind( decl ) == Node_kind::Var_decl )
            {
                visit( ast_.child( decl, 0 ) ); // the type annotation
                visit( ast_.child( decl, 1 ) ); // the initialiser
                continue;
            }

            visit( decl );
        }
        return;
    case Node_kind::Block:
        push_scope();
        for( const Node_id child : ast_.children( id ) )
        {
            visit( child );
        }
        pop_scope();
        return;
    case Node_kind::Destructor_decl:
    case Node_kind::Constructor_decl:
    case Node_kind::Method_decl:
    case Node_kind::Function_decl:
        push_scope( Scope_kind::Barrier );
        visit( ast_.child( id, 3 ) ); // type parameters, before anything that can name one
        visit( ast_.child( id, 0 ) ); // return type; invalid for Destructor_decl
        visit( ast_.child( id, 1 ) ); // param list
        visit( ast_.child( id, 2 ) ); // body
        pop_scope();
        return;
    case Node_kind::Param_decl:
        visit( ast_.child( id, 0 ) ); // the type annotation, which may name a struct
        declare( Symbol_id { ast_.aux( id ) }, id );
        return;
    case Node_kind::Type_param_decl:
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
        visit( ast_.child( id, 0 ) ); // type
        visit( ast_.child( id, 1 ) ); // Var_decl arity of 2: type, initialiser
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
    case Node_kind::Named_type:
    {
        const Node_id decl = lookup( Symbol_id { ast_.aux( id ) } );
        if( decl.is_valid() )
        {
            bindings_[id.v] = decl;
        }

        return; // Not an error when absent
    }
    case Node_kind::Struct_literal:
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

        // Explicit rather than falling into default: the initialiser values are ordinary
        // expressions and still need resolving, and a `return` added here for tidiness would
        // silently stop that happening.
        for( const Node_id init : ast_.children( id ) )
        {
            visit( init );
        }

        return;
    }
    case Node_kind::Case_arm:
    {
        // The arm is the scope, not its body: a pattern's bindings are written *outside* the
        // block - `case Shape::Circle( r ):` - and have to be in scope inside it. One scope over
        // both is what makes `r` visible in the body and invisible in the next arm.
        push_scope();

        const std::span<const Node_id> parts = ast_.children( id );

        for( const Node_id label : parts.subspan( 0, parts.size() - 1 ) )
        {
            if( ast_.kind( label ) != Node_kind::Variant_pattern )
            {
                visit( label );
                continue;
            }

            // The path resolves like any other; the bindings are declarations rather than uses.
            const std::span<const Node_id> pattern = ast_.children( label );

            visit( pattern[0] );

            for( const Node_id binding : pattern.subspan( 1 ) )
            {
                declare( Symbol_id { ast_.aux( binding ) }, binding );
            }
        }

        visit( parts.back() );
        pop_scope();
        return;
    }
    case Node_kind::Enum_decl:
    {
        // Child 0 is the underlying type and is invalid when unwritten, so it cannot go through
        // the default walk - visit() asserts on an invalid id, which is the same convention
        // Var_decl follows for its two optional children.
        //
        // The variant *names* are deliberately not declared. D30 gives them enum-class scoping, so
        // they enter no lexical scope at all: `Colour::Red` is looked up against the enum's own
        // type in the checker, exactly as a field name is, and a bare `Red` stays undeclared.
        //
        // Their payload fields still have to be *visited*, though: `Circle( Point centre )` names a
        // type, and nothing else will resolve it.
        const std::span<const Node_id> children = ast_.children( id );

        push_scope( Scope_kind::Barrier );
        visit( ast_.type_param_list( id ) );

        if( children[1].is_valid() )
        {
            visit( children[1] );
        }

        for( const Node_id variant : children.subspan( 2 ) )
        {
            for( const Node_id field : ast_.children( variant ) )
            {
                visit( ast_.child( field, 0 ) );
            }
        }

        pop_scope();
        return;
    }
    case Node_kind::Class_decl:
    case Node_kind::Struct_decl:
    {
        // Fields are a member namespace, not a lexical one, so for the struct body itself they are
        // deliberately *not* in scopes_: doing that would put `Point` in scope as a field and let
        // it shadow the type `Point` while the fields' own annotations are being resolved. A local
        // map gives the duplicate check without the pollution, and without D19's shadowing walk,
        // which does not apply between members.
        //
        // A destructor body is the exception, and gets them in a scope of its own below - no
        // annotation is ever inside it, so the hazard above cannot arise there. Two passes rather
        // than one, because declaration order carries meaning inside a function body and none
        // between members: a field written after the destructor must still be visible in it.
        // The type parameters, before anything that can name one. A barrier for the reason a
        // function's is: `T` belongs to this declaration and to nothing outside it, and a scope
        // that carried outer names in would let a top-level `T` be found from a member body.
        push_scope( Scope_kind::Barrier );
        visit( ast_.type_param_list( id ) );

        std::unordered_map<u32, Node_id> members;

        for( const Node_id field : ast_.members( id ) )
        {
            if( ast_.kind( field ) != Node_kind::Field_decl )
            {
                continue;
            }

            visit( field ); // the field's type annotation still resolves through the normal path

            const Symbol_id name { ast_.aux( field ) };

            if( !name.is_valid() )
            {
                continue; // the parser already reported the missing name
            }

            const auto [it, inserted] = members.try_emplace( name.v, field );

            if( !inserted )
            {
                error_at(
                    ast_.span( field ),
                    fmt::format( "field `{}` is already declared", interner_.text( name ) ),
                    previous_declaration_note( it->second )
                );
            }
        }

        // Methods join the member scope, so a sibling is callable by bare name: `add( by )` rather
        // than `this.add( by )`, which is what C++ does and what any real class needs - a type whose
        // methods have to qualify each other is tiring to write long before it is large.
        //
        // A second loop rather than a branch in the one above, because a method's own annotations
        // are resolved when its body is visited below, not here.
        for( const Node_id member : ast_.members( id ) )
        {
            if( ast_.kind( member ) != Node_kind::Method_decl )
            {
                continue;
            }

            const Symbol_id name { ast_.aux( member ) };

            if( !name.is_valid() )
            {
                continue;
            }

            const auto [it, inserted] = members.try_emplace( name.v, member );

            if( !inserted && !chain_overload( it->second, member ) )
            {
                error_at(
                    ast_.span( member ),
                    fmt::format( "`{}` is already declared", interner_.text( name ) ),
                    previous_declaration_note( it->second )
                );
            }
        }

        push_scope();

        for( const auto& [name, decl] : members )
        {
            // Straight into the scope rather than through declare(): members are not lexical,
            // duplicates were already reported above, and declare()'s walk reads a barrier scope's
            // names before noticing it is a barrier - so it would call a field that happens to
            // share a top-level name a shadow. The set beside it is what D19's member clause reads.
            scopes_.back().names.emplace( Symbol_id { name }, decl );
            current_fields_.insert( Symbol_id { name } );
        }

        for( const Node_id member : ast_.members( id ) )
        {
            if( is_function_like( ast_.kind( member ) ) )
            {
                visit( member );
            }
        }

        // clear() rather than restoring an enclosing set, because an aggregate cannot nest inside
        // another - parse_aggregate_decl's member loop accepts only fields and destructors. If that
        // ever changes this has to become a save and restore.
        current_fields_.clear();
        pop_scope();

        pop_scope(); // the type parameters

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
        // Two functions of one name are an overload set, not a redeclaration. Whether their
        // signatures actually differ is a question about types, which this pass does not have -
        // the checker asks it once every signature is known.
        if( chain_overload( it->second, decl ) )
        {
            return;
        }

        error_at(
            ast_.span( decl ),
            fmt::format( "`{}` is already declared in this scope", interner_.text( name ) ),
            previous_declaration_note( it->second )
        );
        return;
    }

    // D19: a field may be shadowed by a parameter, never by a local. A parameter naming the field
    // it initialises is forced and idiomatic; a local collision is chosen, and is exactly the
    // silent-wrong-value hazard this rule exists to kill. Checked here rather than through the
    // scope walk because the field scope sits below the member body's barrier - it has to, so a
    // parameter wins the lookup - and the walk stops at that barrier.
    if( ast_.kind( decl ) != Node_kind::Param_decl && current_fields_.contains( name ) )
    {
        error_at(
            ast_.span( decl ),
            fmt::format( "`{}` shadows a field", interner_.text( name ) ),
            "a local may not take a field's name"
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

// Appended at the end of the chain, so walking it gives declaration order - which is what the
// duplicate diagnostic needs to name the *earlier* one.
bool Resolver::chain_overload( Node_id existing, Node_id added )
{
    const Node_kind kind = ast_.kind( existing );

    if( kind != ast_.kind( added ) || ( kind != Node_kind::Function_decl && kind != Node_kind::Method_decl ) )
    {
        return false;
    }

    Node_id last = existing;

    while( next_overload_[last.v].is_valid() )
    {
        last = next_overload_[last.v];
    }

    next_overload_[last.v] = added;

    return true;
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
        ast_          = parse( lex( file_, sm_, interner_, literal_pool_, diags_ ), sm_, diags_ );
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

    const Resolution& resolution() const
    {
        return resolution_;
    }

private:
    Source_manager sm_;
    Interner       interner_;
    Literal_pool   literal_pool_;
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
    // Two functions of one name are an overload set, and whether their signatures differ is a
    // question about types this pass cannot ask. The checker reports the pair that does not.
    SECTION( "two functions with one name are chained rather than reported" )
    {
        const Resolved p( "i32 f() { return 1; }\ni32 f() { return 2; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );

        const Node_id first = p.nth( Node_kind::Function_decl, 0 );

        REQUIRE( p.resolution().next_overload( first ) == p.nth( Node_kind::Function_decl, 1 ) );
        REQUIRE_FALSE( p.resolution().next_overload( p.nth( Node_kind::Function_decl, 1 ) ).is_valid() );
    }

    // The chain is in declaration order, which is what lets a diagnostic name the earlier one.
    SECTION( "three of one name chain in order" )
    {
        const Resolved p( "i32 f() { return 1; }\ni32 f() { return 2; }\ni32 f() { return 3; }\n" );

        const Node_id second = p.resolution().next_overload( p.nth( Node_kind::Function_decl, 0 ) );

        REQUIRE( second == p.nth( Node_kind::Function_decl, 1 ) );
        REQUIRE( p.resolution().next_overload( second ) == p.nth( Node_kind::Function_decl, 2 ) );
    }

    // Only two callables of one kind chain: everything else is still a redeclaration.
    SECTION( "a function and a variable of one name" )
    {
        const Resolved p( "i32 f() { return 1; }\ni32 f = 2;\ni32 main() { return 0; }\n" );

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

TEST_CASE( "resolver_declares_struct_names_at_file_scope", "[sema][resolve]" )
{
    SECTION( "a struct is usable as a type" )
    {
        const Resolved p( "struct Point { f64 x; }\ni32 f( Point p ) { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    // D18 again: declaration order carries no meaning between top-level declarations, so the
    // struct's name has to be collected in the first pass rather than as the walk reaches it.
    SECTION( "even when declared after the function that uses it" )
    {
        const Resolved p( "i32 f( Point p ) { return 0; }\nstruct Point { f64 x; };\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    SECTION( "a struct name collides with a function name" )
    {
        const Resolved p( "i32 Point() { return 0; }\nstruct Point { f64 x; };\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "an unknown type name is left for the checker, not reported here" )
    {
        const Resolved p( "i32 f( Widget w ) { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 ); // `i32` has no declaration either; silence is the only option
    }
}

TEST_CASE( "resolver_reports_duplicate_fields", "[sema][resolve]" )
{
    SECTION( "one message per repeated name, pointing at the first" )
    {
        const Resolved p( "struct Point\n{\n    f64 x;\n    f64 x;\n};\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "field `x` is already declared" ) != std::string::npos );
        REQUIRE( p.rendered().find( "previous declaration is at: 3:" ) != std::string::npos );
    }

    SECTION( "two repeated names give two messages" )
    {
        const Resolved p( "struct P { f64 x; f64 x; f64 y; f64 y; };" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }

    // Fields are a member namespace: a field may share a name with a local, a function, or a type,
    // and none of those is a collision.
    SECTION( "a field does not collide with anything outside the struct" )
    {
        const Resolved p( "struct Point { f64 x; };\n"
                          "i32 count() { return 0; }\n"
                          "struct Other { f64 x; f64 count; f64 Point; };\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }

    // The reverse of the same rule: declaring fields into scopes_ would put `Point` in scope as a
    // field and shadow the type of the same name in the very next field.
    SECTION( "a field named after a type does not shadow that type" )
    {
        const Resolved p( "struct Point { f64 x; };\nstruct Holder { f64 Point; Point inner; };\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
    }
}

// The values inside a struct literal are ordinary expressions. This is the property that breaks
// silently if the Struct_literal case stops visiting its children.
TEST_CASE( "resolver_resolves_inside_struct_literals", "[sema][resolve]" )
{
    SECTION( "the type name binds to its declaration" )
    {
        const Resolved p( "struct Point { f64 x; };\ni32 main() { auto q = Point { 1.0 }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );

        const Node_id literal = p.nth( Node_kind::Struct_literal, 0 );
        REQUIRE( p.declaration_of( literal ) == p.nth( Node_kind::Struct_decl, 0 ) );
    }

    SECTION( "an unknown type is reported" )
    {
        const Resolved p( "i32 main() { auto q = Widget { 1.0 }; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and the initialiser values are resolved too" )
    {
        const Resolved p( "struct Point { f64 x; };\ni32 main() { auto q = Point { missing }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`missing` is not declared" ) != std::string::npos );
    }

    SECTION( "a name used in a literal binds to its declaration" )
    {
        const Resolved p( "struct Point { f64 x; };\ni32 main() { f64 v = 1.0; auto q = Point { v }; return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }
}

// The field name lives in aux precisely so it is never looked up: `p.x` must not send the resolver
// hunting for a variable called `x`.
TEST_CASE( "resolver_does_not_resolve_field_names", "[sema][resolve]" )
{
    const Resolved p( "struct Point { f64 x; };\ni32 f( Point p ) { auto v = p.nonexistent; return 0; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 0 ); // whether the field exists is the checker's question

    const Node_id field = p.nth( Node_kind::Field_expr, 0 );
    REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Param_decl, 0 ) );
    REQUIRE( field.is_valid() );
}

// D18: every top-level declaration is visible throughout the file, so a global is usable above the
// line that declares it, exactly as a function is.
TEST_CASE( "resolver_declares_globals_at_file_scope", "[sema][resolve][globals]" )
{
    SECTION( "a use inside a function resolves to it" )
    {
        const Resolved p( "i32 counter = 1;\ni32 main() { return counter; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }

    SECTION( "even when the function is written above it" )
    {
        const Resolved p( "i32 main() { return counter; }\ni32 counter = 1;\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 0 ) );
    }

    // The trap: a global is declared once by the file-scope pre-pass, and visit() must not declare
    // it a second time. If it does, every global reports "already declared" against itself.
    SECTION( "a global is not declared twice against itself" )
    {
        const Resolved p( "i32 counter = 1;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.rendered().find( "already declared" ) == std::string::npos );
    }

    SECTION( "several globals coexist" )
    {
        const Resolved p( "i32 first = 1;\ni32 second = 2;\ni32 main() { return first + second; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a global and a function may not share a name" )
    {
        const Resolved p( "i32 thing = 1;\ni32 thing() { return 0; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "already declared" ) != std::string::npos );
    }

    SECTION( "nor may two globals" )
    {
        const Resolved p( "i32 thing = 1;\ni32 thing = 2;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // A local of the same name shadows it, and D19's barrier rules are unchanged by the global
    // living in the file scope.
    SECTION( "a local shadows a global" )
    {
        const Resolved p( "i32 counter = 1;\ni32 main() { i32 counter = 2; return counter; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Var_decl, 1 ) );
    }

    SECTION( "a parameter shadows a global too" )
    {
        const Resolved p( "i32 counter = 1;\ni32 take( i32 counter ) { return counter; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Param_decl, 0 ) );
    }

    SECTION( "and its address can be taken" )
    {
        const Resolved p( "i32 counter = 1;\ni32* get() { return &counter; }\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A destructor body sees the fields unqualified, as C++ does. The scope goes up on entering the
// body and down on leaving it, so no field's own type annotation ever sees it - which is what the
// Class_decl case above warns about when it keeps members out of scopes_.
TEST_CASE( "resolver_puts_fields_in_scope_inside_a_destructor", "[sema][resolve][aggregates]" )
{
    SECTION( "a bare field name binds to the field" )
    {
        const Resolved p( "class Buffer { u8* ptr; ~Buffer() { ptr = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Field_decl, 0 ) );
    }

    // Declaration order carries no meaning between members, only within a function body, so a field
    // written after the destructor is still visible inside it.
    SECTION( "a field declared after the destructor is still visible" )
    {
        const Resolved p( "class Buffer { ~Buffer() { ptr = nullptr; } u8* ptr; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "`this` binds to the synthesised parameter" )
    {
        const Resolved p( "class Buffer { u8* ptr; ~Buffer() { this.ptr = nullptr; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 0 ) ) == p.nth( Node_kind::Param_decl, 0 ) );
    }

    SECTION( "the scope does not leak past the body" )
    {
        const Resolved p( "class Buffer { u8* ptr; ~Buffer() { } };\ni32 main() { ptr = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "one class's fields are not visible in another's destructor" )
    {
        const Resolved p( "class A { u8* only_in_a; ~A() { } };\n"
                          "class B { u8* ptr; ~B() { only_in_a = nullptr; } };\n"
                          "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// D19: members are shadowable by a parameter but never by a local. A destructor has no parameters
// of its own, so only the restrictive half is reachable until constructors arrive.
TEST_CASE( "resolver_rejects_a_local_shadowing_a_field", "[sema][resolve][aggregates]" )
{
    SECTION( "a local may not take a field's name" )
    {
        const Resolved p( "class Buffer { u64 len; ~Buffer() { u64 len = 0; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a different name is fine" )
    {
        const Resolved p( "class Buffer { u64 len; ~Buffer() { u64 count = 0; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // File scope is a barrier, and members do not change that: a field may share a top-level name.
    SECTION( "a field may share a name with a top-level declaration" )
    {
        const Resolved p( "i32 count = 0;\nclass Buffer { u64 count; ~Buffer() { } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A constructor body sees the fields exactly as a destructor does - and unlike a destructor it has
// parameters of its own, which is what finally makes D19's member clause reachable.
TEST_CASE( "resolver_puts_fields_in_scope_inside_a_constructor", "[sema][resolve][aggregates]" )
{
    SECTION( "a bare field name binds to the field" )
    {
        const Resolved p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a field declared after the constructor is still visible" )
    {
        const Resolved p( "class Buffer { Buffer( u64 n ) { len = n; } u64 len; };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "`this` binds to the synthesised parameter" )
    {
        const Resolved p( "class Buffer { u64 len; Buffer( u64 n ) { this.len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D19's member clause, live for the first time: a parameter naming the field it initialises is the
// one place the shadow is forced, and `this.len` is how C++ programmers already disambiguate it.
TEST_CASE( "resolver_lets_a_parameter_shadow_a_field", "[sema][resolve][aggregates]" )
{
    SECTION( "a parameter may take a field's name" )
    {
        const Resolved p( "class Buffer { u64 len; Buffer( u64 len ) { this.len = len; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The bare name is the parameter, not the field: the parameter scope sits above the field one.
    SECTION( "the bare name resolves to the parameter" )
    {
        const Resolved p( "class Buffer { u64 len; Buffer( u64 len ) { this.len = len; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.declaration_of( p.nth( Node_kind::Name_expr, 1 ) ) == p.nth( Node_kind::Param_decl, 1 ) );
    }

    SECTION( "a local still may not" )
    {
        const Resolved p( "class Buffer { u64 len; Buffer( u64 n ) { u64 len = n; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
    }
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
