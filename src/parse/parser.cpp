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
    // enclosing is the operator whose recursion we are inside, or End_of_file at the top level.
    // D16 needs it: a tighter operator is consumed inside the looser one's recursion and never
    // appears with a Binary_expr on its left.
    Node_id parse_expression( u8 min_power, Token_kind enclosing = Token_kind::End_of_file );

    // Literals, names, unary operators, and `(` for grouping - which returns the inner node
    // unchanged, so the parens leave no trace.
    Node_id parse_prefix();

    Node_id parse_arg_list();

    void mark_parenthesised( Node_id id );
    bool is_parenthesised( Node_id id ) const;

    std::span<const Token> tokens_;
    u32                    pos_ = 0;
    const Source_manager&  sm_;
    Diagnostics&           diags_;
    Ast                    ast_;

    // Grouping parentheses leave no trace in the tree, so `a & b == c` and `(a & b) == c` produce
    // the same nodes. D16 needs to tell them apart, and parenthesisation is a parsing fact that
    // nothing downstream should see - so it is tracked here, indexed by Node_id, not in the AST.
    std::vector<bool> parenthesised_;
};

// Powers are spaced by 10 so a level can be inserted later without renumbering. 0 means "not an
// infix operator", which is what ends the Pratt loop.
//
// Assignment and `++`/`--` are deliberately absent: both are statements (L11, D12), so `if( x = 5 )`
// and `a[i++]` cannot parse at all rather than needing a special check.
u8 binding_power( Token_kind kind )
{
    switch( kind )
    {
    case Token_kind::Pipe_pipe:
        return 10;

    case Token_kind::Amp_amp:
        return 20;

    case Token_kind::Pipe:
        return 30;

    case Token_kind::Caret:
        return 40;

    case Token_kind::Amp:
        return 50;

    case Token_kind::Equal_equal:
    case Token_kind::Bang_equal:
        return 60;

    case Token_kind::Less:
    case Token_kind::Greater:
    case Token_kind::Less_equal:
    case Token_kind::Greater_equal:
        return 70;

    // Looser than `+` and tighter than `<`, as in C. D16 makes the surprising mixes an error
    // rather than changing this, so no valid C++ silently changes meaning.
    case Token_kind::Less_less:
    case Token_kind::Greater_greater:
        return 80;

    case Token_kind::Plus:
    case Token_kind::Minus:
        return 90;

    case Token_kind::Star:
    case Token_kind::Slash:
    case Token_kind::Percent:
        return 100;

    case Token_kind::L_paren:
        return 110; // function call

    default:
        return 0;
    }
}

// The operator classes D16 refuses to order against each other.
enum class Operator_class
{
    Other,
    Logical_or,
    Logical_and,
    Bitwise,
    Comparison,
    Shift,
    Additive,
};

Operator_class classify( Token_kind kind )
{
    switch( kind )
    {
    case Token_kind::Pipe_pipe:
        return Operator_class::Logical_or;

    case Token_kind::Amp_amp:
        return Operator_class::Logical_and;

    case Token_kind::Amp:
    case Token_kind::Pipe:
    case Token_kind::Caret:
        return Operator_class::Bitwise;

    case Token_kind::Equal_equal:
    case Token_kind::Bang_equal:
    case Token_kind::Less:
    case Token_kind::Greater:
    case Token_kind::Less_equal:
    case Token_kind::Greater_equal:
        return Operator_class::Comparison;

    case Token_kind::Less_less:
    case Token_kind::Greater_greater:
        return Operator_class::Shift;

    case Token_kind::Plus:
    case Token_kind::Minus:
        return Operator_class::Additive;

    default:
        return Operator_class::Other;
    }
}

// The pairs C groups in a way people reliably misread (PLAN §6.3 D16). Concretely, every
// combination below is rejected in both orders unless one side is parenthesised:
//
//   bitwise x comparison    a & b == c    a | b != c    a ^ b < c
//                           a == b & c    a != b | c    a < b ^ c
//     C reads these as  a & (b == c)  - the mistake Ritchie acknowledged.
//
//   shift x additive        a << b + c    a >> b - c
//                           a + b << c    a - b >> c
//     C reads these as  a << (b + c)  - shift is looser than `+`, which almost nobody expects.
//
//   shift x comparison      a << b < c    a >> b == c
//                           a < b << c    a == b >> c
//     C reads these as  (a << b) < c  - correct, but only by luck; the neighbouring case is not.
//
//   && x ||                 a && b || c   a || b && c
//     C reads these as  (a && b) || c  - right, but gcc still warns, and the reader still pauses.
//
// Everything else keeps C++ precedence untouched: arithmetic (`1 + 2 * 3`), comparison chains,
// and same-class runs (`a & b & c`, `a + b + c`) all parse exactly as they do in C++.
bool needs_parentheses( Token_kind inner, Token_kind outer )
{
    const Operator_class a = classify( inner );
    const Operator_class b = classify( outer );

    const auto pair = []( Operator_class x, Operator_class y, Operator_class p, Operator_class q )
    { return ( x == p && y == q ) || ( x == q && y == p ); };

    return pair( a, b, Operator_class::Bitwise, Operator_class::Comparison ) ||
           pair( a, b, Operator_class::Shift, Operator_class::Additive ) ||
           pair( a, b, Operator_class::Shift, Operator_class::Comparison ) ||
           pair( a, b, Operator_class::Logical_and, Operator_class::Logical_or );
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

void Parser::mark_parenthesised( Node_id id )
{
    if( parenthesised_.size() <= id.v )
    {
        parenthesised_.resize( id.v + 1, false );
    }
    parenthesised_[id.v] = true;
}

bool Parser::is_parenthesised( Node_id id ) const
{
    return id.v < parenthesised_.size() && parenthesised_[id.v];
}

Node_id Parser::parse_expression( u8 min_power, Token_kind enclosing )
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

        // function call is a special case: it is the only infix operator that does not produce a
        // Binary_expr node, so it does not need the parentheses check below.
        if( peek().kind == Token_kind::L_paren )
        {
            Node_id args = parse_arg_list();

            left = ast_.add( Node_kind::Call_expr, Span::merge( ast_.span( left ), ast_.span( args ) ), 0, { left, args } );
            continue;
        }

        // D16 has to look both ways. A looser operator folds into `left` first, so `a && b || c`
        // reaches `||` with a Binary_expr(&&) on its left. A tighter one never does: in
        // `a & b == c` the `==` is consumed inside the `&` recursion, which is what `enclosing`
        // catches. Parenthesised operands are unambiguous by construction.
        Token_kind conflicting = Token_kind::End_of_file;

        if( needs_parentheses( enclosing, peek().kind ) )
        {
            conflicting = enclosing;
        }
        else if( ast_.kind( left ) == Node_kind::Binary_expr && !is_parenthesised( left ) &&
                 needs_parentheses( static_cast<Token_kind>( ast_.aux( left ) ), peek().kind ) )
        {
            conflicting = static_cast<Token_kind>( ast_.aux( left ) );
        }

        if( conflicting != Token_kind::End_of_file )
        {
            error_at(
                Span::merge( ast_.span( left ), peek().span ),
                fmt::format(
                    "`{}` and `{}` cannot be mixed without parentheses",
                    token_kind_spelling( conflicting ),
                    token_kind_spelling( peek().kind )
                ),
                "add parentheses to say which grouping you mean"
            );
        }

        const Token op = advance();

        // Left associativity: recurse one above this operator's power, so an equally tight operator
        // to the right belongs to the *next* iteration rather than becoming our right child.
        const Node_id right = parse_expression( power + 1, op.kind );

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

    // Grouping: the inner node is returned unchanged, so the parentheses leave no trace in the
    // tree. Only D16 needs to know they were there, which is what mark_parenthesised records.
    case Token_kind::L_paren:
    {
        advance();
        const Node_id inner = parse_expression( 0 );
        expect( Token_kind::R_paren );
        mark_parenthesised( inner );
        return inner;
    }

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

Node_id Parser::parse_arg_list()
{
    const Span start = peek().span;

    std::vector<Node_id> args;

    if( !expect( Token_kind::L_paren ) )
    {
        return ast_.add( Node_kind::Arg_list, start, 0, args );
    }

    if( !check( Token_kind::R_paren ) )
    {
        do
        {
            args.push_back( parse_expression( 0 ) );
        } while( match( Token_kind::Comma ) );
    }

    expect( Token_kind::R_paren );
    return ast_.add( Node_kind::Arg_list, Span::merge( start, previous().span ), 0, args );
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

// Returns the expression of `return <expr>;` in a one-function program.
Node_id parse_expression_of( const Parsed& p )
{
    const Node_id ret = find_first( p.ast(), p.root(), Node_kind::Return_stmt );
    return ret.is_valid() ? p.child( ret, 0 ) : Node_id {};
}

// A compact shape rendering: "+(1,*(2,3))". Precedence bugs are invisible in a pass/fail but
// obvious here.
std::string shape( const Parsed& p, Node_id id )
{
    if( !id.is_valid() )
    {
        return "<none>";
    }

    if( p.kind( id ) == Node_kind::Binary_expr )
    {
        return std::string( token_kind_spelling( static_cast<Token_kind>( p.aux( id ) ) ) ) + "(" +
               shape( p, p.child( id, 0 ) ) + "," + shape( p, p.child( id, 1 ) ) + ")";
    }

    // "call(f,[1,2])" - the brackets keep the argument list visible as its own node.
    if( p.kind( id ) == Node_kind::Call_expr )
    {
        return "call(" + shape( p, p.child( id, 0 ) ) + "," + shape( p, p.child( id, 1 ) ) + ")";
    }

    if( p.kind( id ) == Node_kind::Arg_list )
    {
        std::string out = "[";
        for( std::size_t i = 0; i < p.children( id ).size(); ++i )
        {
            out += ( i == 0 ? "" : "," ) + shape( p, p.child( id, i ) );
        }
        return out + "]";
    }

    return std::string( p.text( id ) );
}

std::string shape_of( std::string_view expression )
{
    const Parsed p( std::string( "i32 main() { return " ) + std::string( expression ) + "; }" );
    return p.errors().empty() ? shape( p, parse_expression_of( p ) ) : "ERROR";
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

TEST_CASE( "parser_binary_precedence", "[parse]" )
{
    struct Case
    {
        const char* source;
        const char* shape;
    };

    static const Case cases[] = {
        // Multiplicative binds tighter than additive.
        { "1 + 2 * 3", "+(1,*(2,3))" },
        { "1 * 2 + 3", "+(*(1,2),3)" },
        { "1 + 2 / 3", "+(1,/(2,3))" },
        { "1 % 2 - 3", "-(%(1,2),3)" },

        // Additive tighter than shift, shift tighter than relational, relational tighter than
        // equality - each neighbouring pair, in both orders.
        { "a < b + c", "<(a,+(b,c))" },
        { "a + b < c", "<(+(a,b),c)" },
        { "a == b < c", "==(a,<(b,c))" },
        { "a < b == c", "==(<(a,b),c)" },

        // Equality tighter than bitwise-and, and the bitwise chain & then ^ then |.
        { "a & b ^ c", "^(&(a,b),c)" },
        { "a ^ b | c", "|(^(a,b),c)" },
        { "a | b ^ c", "|(a,^(b,c))" },

        // && tighter than nothing here, but tighter than || - checked under D16 instead.
        { "a && b && c", "&&(&&(a,b),c)" },
    };

    for( const Case& c : cases )
    {
        INFO( "source: " << c.source );
        REQUIRE( shape_of( c.source ) == c.shape );
    }
}

// The `power + 1` in parse_expression is what makes this left rather than right. Getting it wrong
// still compiles and still parses - it just computes the wrong answer.
TEST_CASE( "parser_binary_operators_are_left_associative", "[parse]" )
{
    REQUIRE( shape_of( "1 - 2 - 3" ) == "-(-(1,2),3)" );
    REQUIRE( shape_of( "1 - 2 + 3" ) == "+(-(1,2),3)" );
    REQUIRE( shape_of( "8 / 4 / 2" ) == "/(/(8,4),2)" );
    REQUIRE( shape_of( "a % b * c" ) == "*(%(a,b),c)" );
}

TEST_CASE( "parser_binary_operator_is_recorded_in_aux", "[parse]" )
{
    static const Token_kind operators[] = {
        Token_kind::Pipe_pipe,
        Token_kind::Amp_amp,
        Token_kind::Pipe,
        Token_kind::Caret,
        Token_kind::Amp,
        Token_kind::Equal_equal,
        Token_kind::Bang_equal,
        Token_kind::Less,
        Token_kind::Greater,
        Token_kind::Less_equal,
        Token_kind::Greater_equal,
        Token_kind::Less_less,
        Token_kind::Greater_greater,
        Token_kind::Plus,
        Token_kind::Minus,
        Token_kind::Star,
        Token_kind::Slash,
        Token_kind::Percent,
    };

    for( const Token_kind op : operators )
    {
        const std::string source = std::string( "i32 main() { return a " ) + std::string( token_kind_spelling( op ) ) + " b; }";
        const Parsed      p( source );

        INFO( source << "\n" << p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id expression = parse_expression_of( p );
        REQUIRE( p.kind( expression ) == Node_kind::Binary_expr );
        REQUIRE( static_cast<Token_kind>( p.aux( expression ) ) == op );
        REQUIRE( p.children( expression ).size() == 2 );
    }
}

// Grouping returns the inner node unchanged, so the parentheses leave no trace at all.
TEST_CASE( "parser_grouping_produces_no_node", "[parse]" )
{
    REQUIRE( shape_of( "( 1 + 2 )" ) == shape_of( "1 + 2" ) );
    REQUIRE( shape_of( "( ( ( 7 ) ) )" ) == "7" );
    REQUIRE( shape_of( "( 1 + 2 ) * 3" ) == "*(+(1,2),3)" );
    REQUIRE( shape_of( "1 + ( 2 * 3 )" ) == "+(1,*(2,3))" );

    SECTION( "an unclosed group reports once" )
    {
        const Parsed p( "i32 main() { return ( 1 + 2; }" );
        REQUIRE( p.has_errors() );
        INFO( p.errors() );
        REQUIRE( p.errors().find( "expected `)`" ) != std::string::npos );
    }
}

// PLAN §6.3 D16. Both directions matter: a tighter operator is consumed inside the looser one's
// recursion and never appears with a Binary_expr on its left, so the two took different paths.
TEST_CASE( "parser_calls", "[parse]" )
{
    struct Case
    {
        const char* source;
        const char* shape;
    };

    static const Case cases[] = {
        { "f()", "call(f,[])" },
        { "f( 1 )", "call(f,[1])" },
        { "f( 1, 2 )", "call(f,[1,2])" },
        { "f( 1, 2, 3 )", "call(f,[1,2,3])" },

        // Each argument is a full expression, parsed at power 0 - the commas delimit them.
        { "f( a + b )", "call(f,[+(a,b)])" },
        { "f( a, b * c )", "call(f,[a,*(b,c)])" },

        { "f( g( 1 ) )", "call(f,[call(g,[1])])" },
        { "f( g( 1 ), h( 2 ) )", "call(f,[call(g,[1]),call(h,[2])])" },
    };

    for( const Case& c : cases )
    {
        INFO( "source: " << c.source );
        REQUIRE( shape_of( c.source ) == c.shape );
    }
}

// A call binds tighter than every binary operator, so it is always the operand.
TEST_CASE( "parser_calls_bind_tightest", "[parse]" )
{
    REQUIRE( shape_of( "1 + f( 2 )" ) == "+(1,call(f,[2]))" );
    REQUIRE( shape_of( "f( 1 ) + 2" ) == "+(call(f,[1]),2)" );
    REQUIRE( shape_of( "f( 1 ) * g( 2 )" ) == "*(call(f,[1]),call(g,[2]))" );
    REQUIRE( shape_of( "a + b * f( c )" ) == "+(a,*(b,call(f,[c])))" );

    // The call sits on the left of the tighter operator, so both relationships apply at once.
    REQUIRE( shape_of( "1 + f( 5 ) * 3" ) == "+(1,*(call(f,[5]),3))" );
    REQUIRE( shape_of( "f( 1 ) * 2 + 3" ) == "+(*(call(f,[1]),2),3)" );
}

// Chaining falls out of the Pratt loop: after one call, `left` is a Call_expr and the loop goes
// round again.
TEST_CASE( "parser_calls_chain", "[parse]" )
{
    REQUIRE( shape_of( "f( 1 )( 2 )" ) == "call(call(f,[1]),[2])" );
    REQUIRE( shape_of( "f()()" ) == "call(call(f,[]),[])" );
}

TEST_CASE( "parser_call_errors", "[parse]" )
{
    SECTION( "a trailing comma is rejected, as in C++" )
    {
        const Parsed p( "i32 main() { return f( 1, ); }" );
        REQUIRE( p.has_errors() );
        INFO( p.errors() );
        REQUIRE( p.error_count() == 1 );
    }

    SECTION( "an unclosed argument list reports once" )
    {
        const Parsed p( "i32 main() { return f( 1; }" );
        REQUIRE( p.has_errors() );
        INFO( p.errors() );
        REQUIRE( p.errors().find( "expected `)`" ) != std::string::npos );
    }

    SECTION( "a missing argument reports once" )
    {
        const Parsed p( "i32 main() { return f( , 1 ); }" );
        REQUIRE( p.has_errors() );
        INFO( p.errors() );
        REQUIRE( p.errors().find( "expected an expression" ) != std::string::npos );
    }
}

// Call_expr is arity 2 - callee then Arg_list - so the arguments are never spliced in as children.
TEST_CASE( "parser_call_node_shape", "[parse]" )
{
    const Parsed p( "i32 main() { return f( 1, 2 ); }" );

    INFO( p.errors() );
    REQUIRE_FALSE( p.has_errors() );

    const Node_id call = parse_expression_of( p );
    REQUIRE( p.kind( call ) == Node_kind::Call_expr );
    REQUIRE( p.children( call ).size() == 2 );
    REQUIRE( p.kind( p.child( call, 0 ) ) == Node_kind::Name_expr );

    const Node_id args = p.child( call, 1 );
    REQUIRE( p.kind( args ) == Node_kind::Arg_list );
    REQUIRE( p.children( args ).size() == 2 );

    // Arg_list covers both parentheses, matching Param_list.
    REQUIRE( p.text( args ) == "( 1, 2 )" );
    REQUIRE( p.text( call ) == "f( 1, 2 )" );
}

TEST_CASE( "parser_rejects_ambiguous_operator_mixes", "[parse]" )
{
    static const char* rejected[] = {
        "a & b == c",
        "a | b != c",
        "a ^ b < c",
        "a == b & c",
        "a != b | c",
        "a < b ^ c",
        "a << b + c",
        "a >> b - c",
        "a + b << c",
        "a - b >> c",
        "a << b < c",
        "a >> b == c",
        "a < b << c",
        "a == b >> c",
        "a && b || c",
        "a || b && c",
    };

    for( const char* source : rejected )
    {
        const Parsed p( std::string( "i32 main() { return " ) + source + "; }" );

        INFO( "source: " << source << "\n" << p.errors() );
        REQUIRE( p.has_errors() );
        REQUIRE( p.errors().find( "cannot be mixed without parentheses" ) != std::string::npos );
        REQUIRE( p.error_count() == 1 );
    }
}

TEST_CASE( "parser_accepts_the_parenthesised_forms", "[parse]" )
{
    REQUIRE( shape_of( "(a & b) == c" ) == "==(&(a,b),c)" );
    REQUIRE( shape_of( "a & (b == c)" ) == "&(a,==(b,c))" );
    REQUIRE( shape_of( "(a << b) + c" ) == "+(<<(a,b),c)" );
    REQUIRE( shape_of( "a << (b + c)" ) == "<<(a,+(b,c))" );
    REQUIRE( shape_of( "(a && b) || c" ) == "||(&&(a,b),c)" );
    REQUIRE( shape_of( "a && (b || c)" ) == "&&(a,||(b,c))" );
}

// Same-class runs and ordinary arithmetic keep C++ precedence untouched.
TEST_CASE( "parser_leaves_unsurprising_precedence_alone", "[parse]" )
{
    REQUIRE( shape_of( "1 + 2 * 3" ) == "+(1,*(2,3))" );
    REQUIRE( shape_of( "a & b & c" ) == "&(&(a,b),c)" );
    REQUIRE( shape_of( "a + b + c" ) == "+(+(a,b),c)" );
    REQUIRE( shape_of( "a < b == c" ) == "==(<(a,b),c)" );
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
