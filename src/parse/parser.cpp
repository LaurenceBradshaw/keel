#include "parse/parser.h"

#include <fmt/format.h>
#include <cassert>
#include <string>
#include <vector>

namespace keel
{
namespace
{

// Pratt binding power. Higher binds tighter; 0 means "not an infix operator", which ends the loop.
// `++` and `--` are deliberately absent (PLAN §6.3 D12), so `a[i++]` fails with no special case.
u8 binding_power( Token_kind kind );

class Parser
{
public:
    Parser( std::span<const Token> tokens, const Source_manager& sm, Diagnostics& diags );

    Ast run();

private:
    // --- cursor. peek() clamps to the End_of_file token, so no rule needs a bounds check. ---

    const Token& peek( u32 ahead = 0 ) const;

    // The last consumed token. Every rule's span is merge( first token, previous() ).
    const Token& previous() const;

    bool at_end() const;

    const Token& advance();

    bool check( Token_kind kind ) const;
    bool check_keyword( Keyword keyword ) const;

    bool match( Token_kind kind );
    bool match_keyword( Keyword keyword );

    // Consume or report. Never throws - the parser reports and keeps going.
    bool expect( Token_kind kind );

    // --- errors ---

    void error_at( Span span, std::string message, std::string help = {} );

    // "expected `;`, found `,`".
    void error_expected( Token_kind kind );

    // What peek() should be called in a message: its source text where it has one, so "found
    // `widget`" rather than "found `identifier`".
    std::string found_text() const;

    // Panic-mode recovery: skip to something that plausibly starts a new statement, so one mistake
    // does not cascade.
    void synchronise();

    // A failed rule returns this rather than an invalid Node_id, so the tree stays well formed and
    // arity stays fixed. TODO: needs Node_kind::Error adding to node.h.
    Node_id error_node( Span span );

    // --- declarations ---

    Node_id parse_source_file();
    Node_id parse_declaration();
    Node_id parse_function_decl();
    Node_id parse_param_list();

    // --- types. `u32*` is a type *expression* and gets nodes of its own. ---

    Node_id parse_type();
    Node_id parse_param();

    // --- statements ---

    Node_id parse_block();
    Node_id parse_statement();
    Node_id parse_return_stmt();

    // --- expressions ---

    // Pratt: consumes only operators binding at least as tightly as min_power. Left associativity
    // comes from recursing with power + 1.
    Node_id parse_expression( u8 min_power );

    // Literals, names, unary operators, and `(` for grouping - which returns the inner node
    // unchanged, so the parens leave no trace.
    Node_id parse_prefix();

    std::span<const Token> tokens_;
    u32                    pos_ = 0;
    const Source_manager&  sm_;
    Diagnostics&           diags_;
    Ast                    ast_;
};

[[maybe_unused]] u8 binding_power( Token_kind kind )
{
    return 0;
}

Parser::Parser( std::span<const Token> tokens, const Source_manager& sm, Diagnostics& diags )
    : tokens_( tokens ),
      sm_( sm ),
      diags_( diags )
{
}

Ast Parser::run()
{
    parse_source_file();
    return std::move( ast_ );
}

const Token& Parser::peek( u32 ahead ) const
{
    assert( !tokens_.empty() );
    return tokens_[std::min( pos_ + ahead, narrow_cast<u32>( tokens_.size() - 1 ) )];
}

const Token& Parser::previous() const
{
    assert( pos_ > 0 && "previous() called at start of token stream" );
    return tokens_[pos_ - 1];
}

bool Parser::at_end() const
{
    return peek().kind == Token_kind::End_of_file;
}

const Token& Parser::advance()
{
    if( at_end() )
    {
        return peek();
    }
    return tokens_[pos_++];
}

bool Parser::check( Token_kind kind ) const
{
    return peek().kind == kind;
}

bool Parser::check_keyword( Keyword keyword ) const
{
    return peek().kind == Token_kind::Keyword && peek().keyword() == keyword;
}

bool Parser::match( Token_kind kind )
{
    if( check( kind ) )
    {
        advance();
        return true;
    }
    return false;
}

bool Parser::match_keyword( Keyword keyword )
{
    if( check_keyword( keyword ) )
    {
        advance();
        return true;
    }
    return false;
}

bool Parser::expect( Token_kind kind )
{
    if( !match( kind ) )
    {
        error_expected( kind );
        return false;
    }

    return true;
}

void Parser::error_at( Span span, std::string message, std::string help )
{
    diags_.error( span, std::move( message ), std::move( help ) );
}

void Parser::error_expected( Token_kind kind )
{
    // Point just past the last token we accepted, not at the one we found. A missing `;` is missing
    // at the end of the previous line, which is where the reader looks - pointing at the `}` on the
    // next line describes the symptom rather than the mistake.
    const Span at = pos_ > 0 ? Span::point( previous().span.file, previous().span.end ) : peek().span;

    error_at( at, fmt::format( "expected `{}`, found `{}`", token_kind_spelling( kind ), found_text() ) );
}

std::string Parser::found_text() const
{
    if( peek().kind == Token_kind::End_of_file )
    {
        return std::string( token_kind_spelling( Token_kind::End_of_file ) );
    }

    return std::string( sm_.text( peek().span ) );
}

void Parser::synchronise()
{
    // Panic-mode recovery: loop consuming tokens until a plausible statement start.
    // ; -> consume it and stop
    // } -> stop without consuming, enclosing block wants it
    // End_of_file -> stop
    // later, statement keywords -> stop without consuming

    while( !at_end() )
    {
        if( match( Token_kind::Semicolon ) )
        {
            return;
        }

        switch( peek().kind )
        {
        case Token_kind::R_brace:
        case Token_kind::End_of_file:
            return;

        case Token_kind::Keyword:
            switch( peek().keyword() )
            {
            case Keyword::Return:
            case Keyword::If:
            case Keyword::While:
            case Keyword::For:
                return;

            default:
                break;
            }
            break;

        default:
            break;
        }

        advance();
    }
}

Node_id Parser::error_node( Span span )
{
    return ast_.add( Node_kind::Error, span, 0, {} );
}

Node_id Parser::parse_source_file()
{
    // loop until at_end() calling parse_declaration() and adding into a scratch vector
    // warp in a Source_file node spanning the whole file; set_root

    std::vector<Node_id> decls;

    while( !at_end() )
    {
        const u32 before = pos_;

        decls.push_back( parse_declaration() );

        // A rule that reports without consuming would spin here forever. Every loop in the parser
        // needs this guard.
        if( pos_ == before )
        {
            advance();
        }
    }

    // The whole file: the last token is always End_of_file, whose zero-length span sits at
    // text.size(). Span {} would be invalid and assert in the dumper.
    const Span whole = Span::merge( tokens_.front().span, tokens_.back().span );

    const Node_id root = ast_.add( Node_kind::Source_file, whole, 0, decls );
    ast_.set_root( root );
    return root;
}

Node_id Parser::parse_declaration()
{
    // Only functions exist yet. This will dispatch on `struct`, `enum` and `import`, and will need
    // lookahead past the name to tell `i32 f() {}` from `i32 x = 5;`.
    if( check( Token_kind::Identifier ) )
    {
        return parse_function_decl();
    }

    const Span span = peek().span;
    error_at( span, fmt::format( "expected a declaration, found `{}`", found_text() ) );
    synchronise();

    return error_node( span );
}

Node_id Parser::parse_function_decl()
{
    const Span start = peek().span;

    const Node_id return_type = parse_type();

    // The name is a token, not a subtree, so it goes in aux rather than becoming a fourth child.
    Symbol_id name;
    if( check( Token_kind::Identifier ) )
    {
        name = advance().symbol;
    }
    else
    {
        error_expected( Token_kind::Identifier );
    }

    // No early return on a missing name: keep parsing so the body's errors are reported too.
    const Node_id params = parse_param_list();
    const Node_id body   = parse_block();

    // Fixed arity - always these three, even when one of them is an Error node.
    return ast_.add( Node_kind::Function_decl, Span::merge( start, previous().span ), name.v, { return_type, params, body } );
}

Node_id Parser::parse_param_list()
{
    const Span start = peek().span;

    std::vector<Node_id> params;

    if( !expect( Token_kind::L_paren ) )
    {
        // Empty param list, with a span of start..start
        return ast_.add( Node_kind::Param_list, start, 0, params );
    }

    if( !check( Token_kind::R_paren ) )
    {
        do
        {
            params.push_back( parse_param() );
        } while( match( Token_kind::Comma ) );
    }

    expect( Token_kind::R_paren );

    return ast_.add( Node_kind::Param_list, Span::merge( start, previous().span ), 0, params );
}

Node_id Parser::parse_type()
{
    const Span start = peek().span;

    if( !expect( Token_kind::Identifier ) )
    {
        return error_node( start );
    }

    Symbol_id type = previous().symbol;
    return ast_.add( Node_kind::Named_type, Span::merge( start, previous().span ), type.v, {} );
}

Node_id Parser::parse_param()
{
    const Span start = peek().span;

    const Node_id type = parse_type();

    // Same convention as Function_decl: the name is a token, so it goes in aux rather than becoming
    // a second child.
    Symbol_id name;
    if( check( Token_kind::Identifier ) )
    {
        name = advance().symbol;
    }
    else
    {
        error_expected( Token_kind::Identifier );
    }

    return ast_.add( Node_kind::Param_decl, Span::merge( start, previous().span ), name.v, { type } );
}

Node_id Parser::parse_block()
{
    const Span start = peek().span;

    // Report a missing brace but carry on: the statements after it are still worth parsing, and
    // synchronising here would swallow them.
    expect( Token_kind::L_brace );

    std::vector<Node_id> statements;

    while( !check( Token_kind::R_brace ) && !at_end() )
    {
        const u32 before = pos_;

        statements.push_back( parse_statement() );

        // The continuation condition does not itself consume, so a rule that reports without
        // advancing would spin here. Unlike parse_param_list, whose loop requires eating a comma.
        if( pos_ == before )
        {
            advance();
        }
    }

    expect( Token_kind::R_brace );

    return ast_.add( Node_kind::Block, Span::merge( start, previous().span ), 0, statements );
}

Node_id Parser::parse_statement()
{
    if( check_keyword( Keyword::Return ) )
    {
        return parse_return_stmt();
    }

    if( check( Token_kind::L_brace ) )
    {
        return parse_block();
    }

    // Statements are the recovery boundary: this is the one place synchronise() belongs, because
    // its stop set - `;`, `}`, statement keywords - is exactly the set of statement boundaries.
    const Span span = peek().span;

    // A poisoned token: the lexer already reported it, so a second message here would be noise.
    if( !check( Token_kind::Unknown ) )
    {
        error_at( span, fmt::format( "expected a statement, found `{}`", found_text() ) );
    }

    synchronise();

    return error_node( span );
}

Node_id Parser::parse_return_stmt()
{
    // parse_statement only routes here after seeing the keyword, so this is a precondition rather
    // than an error path - and error_expected( Keyword ) could only say "expected `keyword`".
    assert( check_keyword( Keyword::Return ) && "parse_return_stmt called without `return`" );

    const Span start = peek().span;
    advance();

    // Always one child, invalid when there is no value, so the arity stays fixed.
    Node_id value;
    if( !check( Token_kind::Semicolon ) )
    {
        value = parse_expression( 0 );
    }

    expect( Token_kind::Semicolon );

    return ast_.add( Node_kind::Return_stmt, Span::merge( start, previous().span ), 0, { value } );
}

Node_id Parser::parse_expression( u8 min_power )
{
    Node_id left = parse_prefix();

    while( true )
    {
        // 0 means "not an infix operator". Without that test, `0 >= min_power` is true for every
        // token and the loop never ends.
        const u8 power = binding_power( peek().kind );
        if( power == 0 || power < min_power )
        {
            break;
        }

        const Token op = advance();

        // Left associativity: recurse one above this operator's power, so an equally tight operator
        // to the right belongs to the *next* iteration rather than becoming our right child.
        const Node_id right = parse_expression( power + 1 );

        left = ast_.add(
            Node_kind::Binary_expr,
            Span::merge( ast_.span( left ), ast_.span( right ) ),
            static_cast<u32>( op.kind ),
            { left, right }
        );
    }

    return left;
}

Node_id Parser::parse_prefix()
{
    const Span start = peek().span;

    switch( peek().kind )
    {
    case Token_kind::Int_literal:
        advance();
        return ast_.add( Node_kind::Int_literal, Span::merge( start, previous().span ), 0, {} );

    case Token_kind::Float_literal:
        advance();
        return ast_.add( Node_kind::Float_literal, Span::merge( start, previous().span ), 0, {} );

    case Token_kind::String_literal:
        advance();
        return ast_.add( Node_kind::String_literal, Span::merge( start, previous().span ), 0, {} );

    case Token_kind::Char_literal:
        advance();
        return ast_.add( Node_kind::Char_literal, Span::merge( start, previous().span ), 0, {} );

    case Token_kind::Identifier:
        advance();
        return ast_.add( Node_kind::Name_expr, Span::merge( start, previous().span ), previous().symbol.v, {} );

    // `true` and `false` arrive as keywords, not as a literal token kind, so they need their own
    // case. aux carries the value, since one Node_kind covers both.
    case Token_kind::Keyword:
        if( check_keyword( Keyword::True ) || check_keyword( Keyword::False ) )
        {
            const bool value = check_keyword( Keyword::True );
            advance();
            return ast_.add( Node_kind::Bool_literal, Span::merge( start, previous().span ), value ? 1u : 0u, {} );
        }
        break;

    default:
        break;
    }

    // The lexer already reported an Unknown token; do not report it twice.
    if( !check( Token_kind::Unknown ) )
    {
        error_at( peek().span, fmt::format( "expected an expression, found `{}`", found_text() ) );
    }

    return error_node( start );
}

} // namespace

Ast parse( std::span<const Token> tokens, const Source_manager& sm, Diagnostics& diags )
{
    return Parser( tokens, sm, diags ).run();
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "ast/dump.h"
#include "common/interner.h"
#include "lex/lexer.h"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

namespace keel
{
namespace
{

class Parsed
{
public:
    explicit Parsed( std::string_view source )
    {
        file_ = sm_.add_file( "t.kl", std::string( source ) );
        ast_  = parse( lex( file_, sm_, interner_, diags_ ), sm_, diags_ );
    }

    const Ast& ast() const
    {
        return ast_;
    }

    Node_id root() const
    {
        return ast_.root();
    }

    Node_kind kind( Node_id id ) const
    {
        return ast_.kind( id );
    }

    u32 aux( Node_id id ) const
    {
        return ast_.aux( id );
    }

    std::span<const Node_id> children( Node_id id ) const
    {
        return ast_.children( id );
    }

    Node_id child( Node_id id, std::size_t index ) const
    {
        return ast_.children( id )[index];
    }

    std::string_view text( Node_id id ) const
    {
        return sm_.text( ast_.span( id ) );
    }

    bool has_errors() const
    {
        return diags_.has_errors();
    }
    std::size_t error_count() const
    {
        return diags_.error_count();
    }

    std::string errors() const
    {
        std::ostringstream out;
        diags_.render( sm_, out );
        return out.str();
    }

    std::string dump() const
    {
        std::ostringstream out;
        dump_ast( ast_, sm_, interner_, out );
        return out.str();
    }

private:
    Source_manager sm_;
    Interner       interner_;
    Diagnostics    diags_;
    File_id        file_;
    Ast            ast_;
};

// Depth-first search for the first node of a kind, for tests that care about one construct.
Node_id find_first( const Ast& ast, Node_id from, Node_kind wanted )
{
    if( !from.is_valid() )
    {
        return Node_id {};
    }
    if( ast.kind( from ) == wanted )
    {
        return from;
    }

    for( const Node_id child : ast.children( from ) )
    {
        const Node_id found = find_first( ast, child, wanted );
        if( found.is_valid() )
        {
            return found;
        }
    }

    return Node_id {};
}

} // namespace

TEST_CASE( "parser_empty_file_yields_an_empty_source_file", "[parse]" )
{
    const Parsed p( "" );

    REQUIRE( p.root().is_valid() );
    REQUIRE( p.kind( p.root() ) == Node_kind::Source_file );
    REQUIRE( p.children( p.root() ).empty() );
    REQUIRE_FALSE( p.has_errors() );
}

TEST_CASE( "parser_ignores_comments_and_whitespace", "[parse]" )
{
    const Parsed p( "// nothing here\n\n/* nor here */\n" );

    REQUIRE( p.kind( p.root() ) == Node_kind::Source_file );
    REQUIRE( p.children( p.root() ).empty() );
    REQUIRE_FALSE( p.has_errors() );
}

// The M0 vertical slice: declaration, type, param list, block, statement, literal.
TEST_CASE( "parser_minimal_function_shape", "[parse]" )
{
    const Parsed p( "i32 main() { return 0; }" );

    INFO( p.errors() );
    REQUIRE_FALSE( p.has_errors() );

    REQUIRE( p.kind( p.root() ) == Node_kind::Source_file );
    REQUIRE( p.children( p.root() ).size() == 1 );

    const Node_id func = p.child( p.root(), 0 );
    REQUIRE( p.kind( func ) == Node_kind::Function_decl );

    // Fixed arity: return type, parameter list, body.
    REQUIRE( p.children( func ).size() == 3 );
    REQUIRE( p.kind( p.child( func, 0 ) ) == Node_kind::Named_type );
    REQUIRE( p.kind( p.child( func, 1 ) ) == Node_kind::Param_list );
    REQUIRE( p.kind( p.child( func, 2 ) ) == Node_kind::Block );

    REQUIRE( p.text( p.child( func, 0 ) ) == "i32" );
    REQUIRE( p.children( p.child( func, 1 ) ).empty() );

    const Node_id block = p.child( func, 2 );
    REQUIRE( p.children( block ).size() == 1 );

    const Node_id ret = p.child( block, 0 );
    REQUIRE( p.kind( ret ) == Node_kind::Return_stmt );
    REQUIRE( p.children( ret ).size() == 1 );
    REQUIRE( p.kind( p.child( ret, 0 ) ) == Node_kind::Int_literal );
    REQUIRE( p.text( p.child( ret, 0 ) ) == "0" );
}

TEST_CASE( "parser_spans_cover_their_constructs", "[parse]" )
{
    const Parsed p( "i32 main() { return 0; }" );

    const Node_id func = p.child( p.root(), 0 );

    REQUIRE( p.text( func ) == "i32 main() { return 0; }" );
    REQUIRE( p.text( p.child( func, 2 ) ) == "{ return 0; }" );

    const Node_id ret = p.child( p.child( func, 2 ), 0 );
    REQUIRE( p.text( ret ) == "return 0;" );
}

TEST_CASE( "parser_multiple_declarations", "[parse]" )
{
    const Parsed p( "i32 first() { return 1; }\ni32 second() { return 2; }\n" );

    INFO( p.errors() );
    REQUIRE_FALSE( p.has_errors() );
    REQUIRE( p.children( p.root() ).size() == 2 );
    REQUIRE( p.kind( p.child( p.root(), 0 ) ) == Node_kind::Function_decl );
    REQUIRE( p.kind( p.child( p.root(), 1 ) ) == Node_kind::Function_decl );
}

// Fixed arity again: Return_stmt always has one child, invalid when there is no expression.
TEST_CASE( "parser_return_without_a_value", "[parse]" )
{
    const Parsed p( "i32 main() { return; }" );

    INFO( p.errors() );
    REQUIRE_FALSE( p.has_errors() );

    const Node_id ret = find_first( p.ast(), p.root(), Node_kind::Return_stmt );
    REQUIRE( ret.is_valid() );
    REQUIRE( p.children( ret ).size() == 1 );
    REQUIRE_FALSE( p.child( ret, 0 ).is_valid() );
}

// A compound statement is a statement, as in C++. Without this, synchronise() consumed the inner
// `{`, the block closed on the inner `}`, and the outer `}` was orphaned at top level.
TEST_CASE( "parser_literals", "[parse]" )
{
    struct Case
    {
        const char* expression;
        Node_kind   kind;
    };

    static const Case cases[] = {
        { "42", Node_kind::Int_literal },
        { "0", Node_kind::Int_literal },
        { "0xFF", Node_kind::Int_literal },
        { "0b1010", Node_kind::Int_literal },
        { "1_000", Node_kind::Int_literal },
        { "1.5", Node_kind::Float_literal },
        { "1e10", Node_kind::Float_literal },
        { "1.5e-3", Node_kind::Float_literal },
        { "\"\"", Node_kind::String_literal },
        { "\"hello\"", Node_kind::String_literal },
        { "'a'", Node_kind::Char_literal },
        { "'\\n'", Node_kind::Char_literal },
        { "true", Node_kind::Bool_literal },
        { "false", Node_kind::Bool_literal },
    };

    for( const Case& c : cases )
    {
        const Parsed p( std::string( "i32 main() { return " ) + c.expression + "; }" );

        INFO( "expression " << c.expression << "\n" << p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id literal = p.child( find_first( p.ast(), p.root(), Node_kind::Return_stmt ), 0 );
        REQUIRE( literal.is_valid() );
        REQUIRE( p.kind( literal ) == c.kind );
        REQUIRE( p.text( literal ) == c.expression );
        REQUIRE( p.children( literal ).empty() );
    }
}

// One Node_kind covers both, so the value has to live in aux - the span text is for humans.
TEST_CASE( "parser_bool_literal_records_its_value", "[parse]" )
{
    const Parsed yes( "i32 main() { return true; }" );
    const Parsed no( "i32 main() { return false; }" );

    REQUIRE( yes.aux( find_first( yes.ast(), yes.root(), Node_kind::Bool_literal ) ) == 1 );
    REQUIRE( no.aux( find_first( no.ast(), no.root(), Node_kind::Bool_literal ) ) == 0 );
}

// Only `true` and `false` are literals; every other keyword must still fail as an expression.
TEST_CASE( "parser_other_keywords_are_not_literals", "[parse]" )
{
    for( const std::string_view keyword : { "if", "while", "struct", "return", "const" } )
    {
        const Parsed p( std::string( "i32 main() { return " ) + std::string( keyword ) + "; }" );

        INFO( "keyword " << keyword );
        REQUIRE( p.has_errors() );
        REQUIRE( p.errors().find( "expected an expression" ) != std::string::npos );
    }
}

TEST_CASE( "parser_name_expression", "[parse]" )
{
    SECTION( "an identifier becomes a Name_expr" )
    {
        const Parsed p( "i32 main() { return x; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id ret = find_first( p.ast(), p.root(), Node_kind::Name_expr );
        REQUIRE( ret.is_valid() );
        REQUIRE( p.text( ret ) == "x" );
        REQUIRE( p.children( ret ).empty() );
    }

    // The span alone would be enough to print the name, but the resolver compares Symbol_ids, so
    // aux has to carry it.
    SECTION( "the symbol is recorded in aux" )
    {
        const Parsed p( "i32 main() { return widget; }" );

        const Node_id name = find_first( p.ast(), p.root(), Node_kind::Name_expr );
        REQUIRE( Symbol_id { p.aux( name ) }.is_valid() );

        // A different identifier must intern to a different symbol.
        const Node_id func = p.child( p.root(), 0 );
        REQUIRE( p.aux( name ) != p.aux( func ) );
    }

    SECTION( "the same spelling interns to the same symbol" )
    {
        const Parsed p( "i32 f( i32 value ) { return value; }" );

        const Node_id param = find_first( p.ast(), p.root(), Node_kind::Param_decl );
        const Node_id name  = find_first( p.ast(), p.root(), Node_kind::Name_expr );

        REQUIRE( param.is_valid() );
        REQUIRE( name.is_valid() );
        REQUIRE( p.aux( param ) == p.aux( name ) );
    }

    // `if` is Token_kind::Keyword, not Identifier, so it must not slip through as a name.
    SECTION( "a keyword is not a name" )
    {
        const Parsed p( "i32 main() { return if; }" );

        REQUIRE( p.has_errors() );
        INFO( p.errors() );
        REQUIRE( p.errors().find( "expected an expression" ) != std::string::npos );
    }
}

TEST_CASE( "parser_nested_blocks", "[parse]" )
{
    SECTION( "adjacent braces" )
    {
        const Parsed p( "i32 main() {{}}" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id body = p.child( p.child( p.root(), 0 ), 2 );
        REQUIRE( p.kind( body ) == Node_kind::Block );
        REQUIRE( p.children( body ).size() == 1 );
        REQUIRE( p.kind( p.child( body, 0 ) ) == Node_kind::Block );

        // The outer block must cover both closing braces, not stop at the inner one.
        REQUIRE( p.text( body ) == "{{}}" );
    }

    SECTION( "with statements inside" )
    {
        const Parsed p( "i32 main() { { return 0; } }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id outer = p.child( p.child( p.root(), 0 ), 2 );
        const Node_id inner = p.child( outer, 0 );

        REQUIRE( p.kind( inner ) == Node_kind::Block );
        REQUIRE( p.children( inner ).size() == 1 );
        REQUIRE( p.kind( p.child( inner, 0 ) ) == Node_kind::Return_stmt );
    }

    SECTION( "three deep" )
    {
        const Parsed p( "i32 main() { { { } } }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        Node_id level = p.child( p.child( p.root(), 0 ), 2 );
        for( int depth = 0; depth < 2; ++depth )
        {
            INFO( "depth " << depth );
            REQUIRE( p.children( level ).size() == 1 );
            level = p.child( level, 0 );
            REQUIRE( p.kind( level ) == Node_kind::Block );
        }
    }

    SECTION( "an unclosed nested block still reports once" )
    {
        const Parsed p( "i32 main() { { }" );

        REQUIRE( p.has_errors() );
        INFO( p.errors() );
        REQUIRE( p.errors().find( "expected `}`" ) != std::string::npos );
    }
}

TEST_CASE( "parser_dump_matches_the_tree", "[parse]" )
{
    const Parsed p( "i32 main() { return 0; }" );

    const std::string out = p.dump();

    INFO( out );
    REQUIRE( out.find( "Source_file" ) != std::string::npos );
    REQUIRE( out.find( "  Function_decl" ) != std::string::npos );
    REQUIRE( out.find( "    Named_type" ) != std::string::npos );
    REQUIRE( out.find( "    Param_list" ) != std::string::npos );
    REQUIRE( out.find( "    Block" ) != std::string::npos );
    REQUIRE( out.find( "      Return_stmt" ) != std::string::npos );
    REQUIRE( out.find( "        Int_literal" ) != std::string::npos );
}

TEST_CASE( "parser_reports_a_missing_semicolon", "[parse]" )
{
    const Parsed p( "i32 main() { return 0 }" );

    REQUIRE( p.has_errors() );
    INFO( p.errors() );
    REQUIRE( p.errors().find( "expected `;`" ) != std::string::npos );
}

TEST_CASE( "parser_reports_a_missing_brace", "[parse]" )
{
    const Parsed p( "i32 main() { return 0;" );

    REQUIRE( p.has_errors() );
    INFO( p.errors() );
    REQUIRE( p.errors().find( "expected `}`" ) != std::string::npos );
}

// One mistake must not cascade: the declaration after the bad one still parses.
TEST_CASE( "parser_recovers_and_keeps_parsing", "[parse]" )
{
    const Parsed p( "i32 first() { return 0 }\ni32 second() { return 1; }\n" );

    REQUIRE( p.has_errors() );
    INFO( p.errors() );
    REQUIRE( p.children( p.root() ).size() == 2 );
    REQUIRE( p.kind( p.child( p.root(), 1 ) ) == Node_kind::Function_decl );
}

TEST_CASE( "parser_marks_failed_subtrees_with_an_error_node", "[parse]" )
{
    const Parsed p( "i32 main() { return @; }" );

    REQUIRE( p.has_errors() );
    INFO( p.dump() );
    REQUIRE( find_first( p.ast(), p.root(), Node_kind::Error ).is_valid() );
}

// The signature parser bug is a loop whose body reports an error without consuming anything. These
// hang rather than fail if that happens, which is a loud enough signal.
TEST_CASE( "parser_terminates_on_adversarial_input", "[parse]" )
{
    for( const std::string_view source : { "}", "}}}}", ";;;;", ")", "@@@@", "i32", "i32 main(", "{{{{", "return" } )
    {
        INFO( "source '" << source << "'" );
        const Parsed p( source );
        REQUIRE( p.root().is_valid() ); // reached only if parsing terminated
    }
}

TEST_CASE( "parser_reports_each_mistake_once", "[parse]" )
{
    const Parsed p( "i32 main() { return 0 }" );

    INFO( p.errors() );
    REQUIRE( p.error_count() == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
