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
// Above every binary operator and below a call, so `-a * b` is `(-a) * b` while `-f( x )` is
// `-(f(x))`. Not part of binding_power: unary operators are prefix, not infix.
constexpr u8 k_unary_power = 105;

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
    void error_expected( Token_kind kind, std::string help = {} );

    // A declared name, or nothing plus a report. A keyword found here was meant as the name, so it
    // is consumed as well: leaving it in place trips every rule after this one.
    Symbol_id expect_name();

    // What peek() should be called in a message: its source text where it has one, so "found
    // `widget`" rather than "found `identifier`".
    std::string found_text() const;

    // Panic-mode recovery: skip to something that plausibly starts a new statement, so one mistake
    // does not cascade.
    void synchronise();

    // A failed rule returns this rather than an invalid Node_id, so the tree stays well formed and
    // arity stays fixed.
    Node_id error_node( Span span );

    // --- declarations ---

    Node_id parse_source_file();
    Node_id parse_declaration();
    Node_id parse_function_decl();
    Node_id parse_param_list( Node_id leading = Node_id {} );
    Node_id parse_aggregate_decl();
    Node_id parse_enum_decl();
    Node_id parse_variant_decl();
    Node_id parse_field_decl();
    Node_id parse_destructor_decl( Symbol_id enclosing );
    Node_id parse_constructor_decl( Symbol_id enclosing );

    // `Point { 0.0, 0.0 }` or `Point { .x = 0.0, .y = 0.0 }`. Entered from parse_prefix once the
    // identifier is consumed and a `{` is seen behind it.
    Node_id parse_struct_literal( Span start, Symbol_id type_name );

    // One initialiser. aux carries the field name in the designated form and stays invalid in the
    // positional one - Symbol_id has its own sentinel, so "no name" is representable rather than
    // borrowed from a real id.
    Node_id parse_field_init();

    // --- types. `u32*` is a type *expression* and gets nodes of its own. ---

    Node_id parse_type();
    Node_id parse_type_with_mode();
    Node_id parse_param();

    // --- statements ---

    Node_id parse_block();
    Node_id parse_statement();

    // Decides whether a statement starting with an identifier is a declaration. A scan, not a trial
    // parse: it moves the cursor, looks, and puts it back, building no nodes and reporting nothing.
    // A speculative *parse* would emit diagnostics for guesses that turn out wrong.
    bool looks_like_declaration();

    bool looks_like_binding();

    // Scans a type followed by the declared name, leaving pos_ just after that name. Shared by
    // looks_like_declaration, which only wants the answer, and parse_declaration, which needs to
    // see what follows the name.
    bool scan_type_and_name();

    // Whether peek() could begin an expression. Keeps "expected a statement" for tokens that
    // cannot, rather than letting parse_prefix report the vaguer "expected an expression".
    bool can_start_expression() const;

    // Whether peek() begins exactly where the previous token ended, with no space between. Spans
    // are byte offsets, so this is the whole test. PLAN §6.3 D17 uses it to bind `*` to the type:
    // `u32* p` is a pointer declaration, `u32 * p` is a multiplication.
    bool peek_is_adjacent() const;

    // `Vector<Vector<i32>>` closes with a single `>>` token - the lexer is right to produce it
    // (lexer_generic_close_is_two_greaters), so the parser splits it here rather than mutating the
    // token stream. Returns true when one closing `>` was consumed.
    bool    match_generic_close();
    Node_id parse_return_stmt();
    Node_id parse_var_decl();
    Node_id parse_expression_stmt( bool consume_semicolon = true );

    Node_id parse_if_stmt();
    Node_id parse_while_stmt();
    Node_id parse_for_stmt();

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
    Node_id parse_keyword_prefix( Span start );

    Node_id parse_arg_list();

    void mark_parenthesised( Node_id id );
    bool is_parenthesised( Node_id id ) const;

    Node_id synthesise_receiver( Symbol_id enclosing, Span span );

    bool at_mode_keyword() const;

    std::span<const Token> tokens_;
    u32                    pos_ = 0;
    const Source_manager&  sm_;
    Diagnostics&           diags_;
    Ast                    ast_;

    // Half of a `>>` that has already been consumed while closing a generic argument list.
    u32 pending_greater_ = 0;

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
// How a token kind reads in "expected ...". Punctuation is quoted because it is what the author
// would type; a category is prose with its own article, because "identifier" is not something you
// can write.
std::string expectation( Token_kind kind )
{
    switch( kind )
    {
    case Token_kind::Identifier:
        return "an identifier";
    case Token_kind::Int_literal:
        return "an integer literal";
    case Token_kind::Float_literal:
        return "a floating-point literal";
    case Token_kind::String_literal:
        return "a string literal";
    case Token_kind::Char_literal:
        return "a character literal";
    case Token_kind::Keyword:
        return "a keyword";
    case Token_kind::End_of_file:
        return "end of file";
    case Token_kind::Unknown:
        return "a valid character";
    default:
        return fmt::format( "`{}`", token_kind_spelling( kind ) );
    }
}

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

    // Postfix, and tighter than every operator: `a.x + b.y` is `(a.x) + (b.y)`, and `f().x`
    // and `p.x.y` chain through the same loop.
    case Token_kind::L_paren:     // function call
    case Token_kind::Dot:         // field access
    case Token_kind::Colon_colon: // scope resolution
    case Token_kind::Arrow:       // not an operator, but it has to be reached to be rejected (D22)
        return 110;

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

bool is_assignment( Token_kind kind )
{
    switch( kind )
    {
    case Token_kind::Equal:
    case Token_kind::Plus_equal:
    case Token_kind::Minus_equal:
    case Token_kind::Star_equal:
    case Token_kind::Slash_equal:
    case Token_kind::Percent_equal:
    case Token_kind::Amp_equal:
    case Token_kind::Pipe_equal:
    case Token_kind::Caret_equal:
    case Token_kind::Less_less_equal:
    case Token_kind::Greater_greater_equal:
        return true;

    default:
        return false;
    }
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

void Parser::error_expected( Token_kind kind, std::string help )
{
    // Point just past the last token we accepted, not at the one we found. A missing `;` is missing
    // at the end of the previous line, which is where the reader looks - pointing at the `}` on the
    // next line describes the symptom rather than the mistake.
    const Span at = pos_ > 0 ? Span::point( previous().span.file, previous().span.end ) : peek().span;

    // A keyword where a name was wanted is worth saying out loud. `out`, `ref` and `move` are
    // ordinary identifiers in C++, so a program can arrive here without its author suspecting that
    // the name is the problem.
    if( help.empty() && kind == Token_kind::Identifier && check( Token_kind::Keyword ) )
    {
        help = fmt::format( "{} is a keyword, so it cannot be used as a name", found_text() );
    }

    error_at( at, fmt::format( "expected {}, found {}", expectation( kind ), found_text() ), std::move( help ) );
}

Symbol_id Parser::expect_name()
{
    if( expect( Token_kind::Identifier ) )
    {
        return previous().symbol;
    }

    if( check( Token_kind::Keyword ) )
    {
        advance();
    }

    return Symbol_id {};
}

std::string Parser::found_text() const
{
    // Quoted, because it is text the author actually wrote - except at the end of the file, where
    // there is nothing to quote and "found `end of file`" reads as though they typed that.
    if( peek().kind == Token_kind::End_of_file )
    {
        return std::string( token_kind_spelling( Token_kind::End_of_file ) );
    }

    return fmt::format( "`{}`", sm_.text( peek().span ) );
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
    if( check_keyword( Keyword::Struct ) || check_keyword( Keyword::Class ) )
    {
        return parse_aggregate_decl();
    }

    if( check_keyword( Keyword::Enum ) )
    {
        return parse_enum_decl();
    }

    // A function and a file-scope variable both open with a type and a name; only what follows the
    // name separates them. `(` opens a parameter list, anything else belongs to a variable.
    //
    // A failed scan takes the variable path deliberately: `i32 = 1;` is a variable missing its
    // name, and parse_var_decl says so, where parse_function_decl would complain about a missing
    // `(` instead.
    // A mode may open one too: `const ref T f( ... )` and `ref T f( ... )` are declarations, and
    // scan_type_and_name below steps over the mode to find the name.
    if( check( Token_kind::Identifier ) || check_keyword( Keyword::Const ) || at_mode_keyword() )
    {
        const u32  saved    = pos_;
        const bool function = scan_type_and_name() && check( Token_kind::L_paren );

        pos_ = saved;

        return function ? parse_function_decl() : parse_var_decl();
    }

    const Span span = peek().span;
    error_at( span, fmt::format( "expected a declaration, found {}", found_text() ) );
    synchronise();

    return error_node( span );
}

Node_id Parser::parse_function_decl()
{
    const Span start = peek().span;

    const Node_id return_type = parse_type_with_mode();

    // The name is a token, not a subtree, so it goes in aux rather than becoming a fourth child.
    const Symbol_id name = expect_name();

    // No early return on a missing name: keep parsing so the body's errors are reported too.
    const Node_id params = parse_param_list();

    // A `;` in place of the body is a forward declaration, which D18 makes unnecessary. Return an
    // Error node rather than a Function_decl: this declares nothing, so letting it through would
    // also make the real definition below it look like a duplicate. Stopping here likewise keeps
    // parse_block from reading the following declarations as statements.
    if( check( Token_kind::Semicolon ) )
    {
        error_expected(
            Token_kind::L_brace, "Keel has no forward declarations: every top-level declaration is visible throughout the file"
        );
        return error_node( Span::merge( start, advance().span ) );
    }

    const Node_id body = parse_block();

    // Fixed arity - always these three, even when one of them is an Error node.
    // A declaration with no name is not one: sema reads every declaration's name to report
    // about it, so handing one over means a keyword or a missing identifier crashes a pass
    // that had no reason to expect it. Everything inside was still parsed, so the errors in
    // there are already reported.
    if( !name.is_valid() )
    {
        return error_node( Span::merge( start, previous().span ) );
    }

    return ast_.add( Node_kind::Function_decl, Span::merge( start, previous().span ), name.v, { return_type, params, body } );
}

Node_id Parser::parse_param_list( Node_id leading )
{
    const Span start = peek().span;

    std::vector<Node_id> params;

    if( leading.is_valid() )
    {
        params.push_back( leading );
    }

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

Node_id Parser::parse_aggregate_decl()
{
    const Span start    = peek().span;
    const bool is_class = match_keyword( Keyword::Class );

    match_keyword( is_class ? Keyword::Class : Keyword::Struct );

    // The name is a token, so it goes in aux rather than becoming a child.
    const Symbol_id name = expect_name();

    // Bail rather than carry on: with no brace there is no field list to find, and scanning for
    // one runs to the next `}` - which belongs to whatever encloses this.
    if( !expect( Token_kind::L_brace ) )
    {
        // `struct x;` is C's opaque forward declaration. Swallow the terminator so it does not
        // come back as a second complaint about a stray `;`.
        match( Token_kind::Semicolon );

        return error_node( Span::merge( start, previous().span ) );
    }

    std::vector<Node_id> members;
    while( !check( Token_kind::R_brace ) && !at_end() )
    {
        const u32 before = pos_;

        const bool is_destructor  = check( Token_kind::Tilde );
        const bool is_constructor = check( Token_kind::Identifier ) && peek( 1 ).kind == Token_kind::L_paren;

        members.push_back(
            is_destructor    ? parse_destructor_decl( name )
            : is_constructor ? parse_constructor_decl( name )
                             : parse_field_decl()
        );

        if( pos_ == before )
        {
            advance();
        }
    }

    expect( Token_kind::R_brace );
    expect( Token_kind::Semicolon );

    // A declaration with no name is not one: sema reads every declaration's name to report
    // about it, so handing one over means a keyword or a missing identifier crashes a pass
    // that had no reason to expect it. Everything inside was still parsed, so the errors in
    // there are already reported.
    if( !name.is_valid() )
    {
        return error_node( Span::merge( start, previous().span ) );
    }

    return ast_.add(
        is_class ? Node_kind::Class_decl : Node_kind::Struct_decl, Span::merge( start, previous().span ), name.v, members
    );
}

Node_id Parser::parse_enum_decl()
{
    const Span start = peek().span;
    match_keyword( Keyword::Enum );

    // D30: one `enum` keyword. `enum class E` is recognised so the diagnostic can name the fix,
    // which is D22's pattern for `->`.
    if( check_keyword( Keyword::Class ) )
    {
        error_at( peek().span, "`enum class` is not a spelling in Keel", "write `enum` - it already means this" );
        advance();
    }

    const Symbol_id name = expect_name();

    // D30: the underlying type is spelled as in C++. parse_type() rather than a bare name, so
    // `enum E : Colour` is a *sema* error about the type rather than a parse error about a token.
    const Node_id underlying = match( Token_kind::Colon ) ? parse_type() : Node_id {};

    expect( Token_kind::L_brace );

    // Child 0 is the underlying type and is invalid when unwritten, which is the convention
    // Var_decl already uses for a missing annotation - so the variants start at 1 and a consumer
    // never has to ask which kind the last child is.
    std::vector<Node_id> members { underlying };
    while( !check( Token_kind::R_brace ) && !at_end() )
    {
        const u32 before = pos_;

        members.push_back( parse_variant_decl() );

        if( !check( Token_kind::R_brace ) )
        {
            expect( Token_kind::Comma );
        }
        else if( check( Token_kind::Comma ) )
        {
            advance();
        }

        if( pos_ == before )
        {
            advance();
        }
    }

    expect( Token_kind::R_brace );
    expect( Token_kind::Semicolon );
    return ast_.add( Node_kind::Enum_decl, Span::merge( start, previous().span ), name.v, members );
}

Node_id Parser::parse_variant_decl()
{
    const Span      start = peek().span;
    const Symbol_id name  = expect_name();
    return ast_.add( Node_kind::Variant_decl, Span::merge( start, previous().span ), name.v, {} );
}

Node_id Parser::parse_field_decl()
{
    const Span start = peek().span;

    const Node_id type = parse_type();

    // Same convention as Function_decl: the name is a token, so it goes in aux rather than becoming
    // a second child.
    const Symbol_id name = expect_name();

    expect( Token_kind::Semicolon );

    // A declaration with no name is not one: sema reads every declaration's name to report
    // about it, so handing one over means a keyword or a missing identifier crashes a pass
    // that had no reason to expect it. Everything inside was still parsed, so the errors in
    // there are already reported.
    if( !name.is_valid() )
    {
        return error_node( Span::merge( start, previous().span ) );
    }

    return ast_.add( Node_kind::Field_decl, Span::merge( start, previous().span ), name.v, { type } );
}

Node_id Parser::parse_destructor_decl( Symbol_id enclosing )
{
    const Span start = peek().span;

    expect( Token_kind::Tilde );

    const Symbol_id name = expect_name();

    // C++ writes `this` implicitly; Keel writes it down. As an ordinary parameter it needs no
    // special case in the resolver, the checker, mangling or lowering - it is simply parameter 0.
    const Node_id receiver = synthesise_receiver( enclosing, start );
    // Parsed rather than rejected on sight, so a stray parameter does not desynchronise the body.
    const Node_id params = parse_param_list( receiver );

    if( ast_.children( params ).size() > 1 ) // only receiver is implicitly allowed.
    {
        error_at( ast_.span( params ), "destructors take no parameters" );
    }

    const Node_id body = parse_block();

    // Same rule as every other declaration: sema reads a name to report about it, so a nameless one
    // crashes a pass that had no reason to expect it.
    if( !name.is_valid() )
    {
        return error_node( Span::merge( start, previous().span ) );
    }

    return ast_.add( // no return type in children
        Node_kind::Destructor_decl,
        Span::merge( start, previous().span ),
        name.v,
        { Node_id {}, params, body }

    );
}

Node_id Parser::parse_constructor_decl( Symbol_id enclosing )
{
    const Span      start = peek().span;
    const Symbol_id name  = expect_name();

    // Same receiver the destructor gets, and for the same reason: as an ordinary parameter it
    // needs no special case in any later pass.
    const Node_id receiver = synthesise_receiver( enclosing, start );
    const Node_id params   = parse_param_list( receiver );
    const Node_id body     = parse_block();

    if( !name.is_valid() )
    {
        return error_node( Span::merge( start, previous().span ) );
    }

    return ast_.add( Node_kind::Constructor_decl, Span::merge( start, previous().span ), name.v, { Node_id {}, params, body } );
}

Node_id Parser::parse_struct_literal( Span start, Symbol_id type_name )
{
    expect( Token_kind::L_brace );

    std::vector<Node_id> initialisers;

    while( !check( Token_kind::R_brace ) && !at_end() )
    {
        const u32 before = pos_;

        initialisers.push_back( parse_field_init() );

        // Same guard as parse_block: a rule that reports without advancing would spin here.
        if( pos_ == before )
        {
            advance();
            continue;
        }

        // A trailing comma is allowed, unlike in an argument list - C++ permits one in a braced
        // initialiser and rejects one in a call, and §5.1 says to follow it rather than to be
        // internally tidy.
        if( !match( Token_kind::Comma ) )
        {
            break;
        }
    }

    expect( Token_kind::R_brace );

    return ast_.add( Node_kind::Struct_literal, Span::merge( start, previous().span ), type_name.v, initialisers );
}

Node_id Parser::parse_field_init()
{
    const Span start = peek().span;

    Symbol_id name; // left invalid by the positional form

    if( match( Token_kind::Dot ) )
    {
        // Only on success: a failed expect() does not advance, so previous() would be the dot.
        if( expect( Token_kind::Identifier ) )
        {
            name = previous().symbol;
        }

        expect( Token_kind::Equal );
    }

    const Node_id value = parse_expression( 0 );

    return ast_.add( Node_kind::Field_init, Span::merge( start, previous().span ), name.v, { value } );
}

Node_id Parser::parse_type()
{
    const Span start = peek().span;

    // A leading const wraps the whole type: `const i32`. looks_like_declaration accepts this, so
    // parse_type has to as well, or the scan and the parse disagree.
    const bool leading_const = match_keyword( Keyword::Const );

    if( !expect( Token_kind::Identifier ) )
    {
        return error_node( start );
    }

    const Symbol_id name = previous().symbol;

    // The Named_type covers only the identifier: a leading const belongs to the Const_type that
    // wraps it, not to the name.
    Node_id type = ast_.add( Node_kind::Named_type, previous().span, name.v, {} );

    // Generic arguments. Nesting works because match_generic_close splits `>>`.
    if( check( Token_kind::Less ) )
    {
        const Span           open = peek().span;
        std::vector<Node_id> arguments;

        advance();

        if( !check( Token_kind::Greater ) && !check( Token_kind::Greater_greater ) )
        {
            do
            {
                arguments.push_back( parse_type() );
            } while( match( Token_kind::Comma ) );
        }

        if( !match_generic_close() )
        {
            error_expected( Token_kind::Greater );
        }

        const Node_id list = ast_.add( Node_kind::Type_arg_list, Span::merge( open, previous().span ), 0, arguments );

        type = ast_.add( Node_kind::Generic_type, Span::merge( start, previous().span ), 0, { type, list } );
    }

    if( leading_const )
    {
        type = ast_.add( Node_kind::Const_type, Span::merge( start, previous().span ), 0, { type } );
    }

    while( true )
    {
        // A trailing const applies to what precedes it: `i32* const p` is a const pointer.
        if( match_keyword( Keyword::Const ) )
        {
            type = ast_.add( Node_kind::Const_type, Span::merge( start, previous().span ), 0, { type } );
            continue;
        }

        if( !check( Token_kind::Star ) && !check( Token_kind::Amp ) )
        {
            break;
        }

        // D32: `&` means address-of and nothing else. Reported before D17's adjacency check below,
        // or `i32 &r` would give two diagnostics for one mistake - and the first of them would be
        // advice about how to space a spelling that no longer exists.
        if( check( Token_kind::Amp ) )
        {
            error_at( peek().span, "`&` is not a type", "write `ref T` for a reference binding" );
            advance();
            continue;
        }

        // D17: `*` belongs to the type, so it must touch it. `u32 *p` is rejected rather than
        // silently read as a multiplication, which is what C's declarator syntax does. `&` is no
        // longer among them, which is why this reads as one token rather than two.
        if( !peek_is_adjacent() )
        {
            error_at(
                peek().span,
                "`*` must touch the type it modifies",
                fmt::format( "write `{}*` rather than `{} *`", sm_.text( ast_.span( type ) ), sm_.text( ast_.span( type ) ) )
            );
        }

        advance();

        type = ast_.add( Node_kind::Pointer_type, Span::merge( start, previous().span ), 0, { type } );
    }

    return type;
}

Node_id Parser::parse_type_with_mode()
{
    const Span start = peek().span;

    // `const ref T`, in that order and only that order. Looked for before the mode so the const
    // ends up outermost, which is what makes it the *binding* that is const rather than the type.
    const bool leading_const = check_keyword( Keyword::Const ) && peek( 1 ).keyword() == Keyword::Ref;

    if( leading_const )
    {
        advance();
    }

    if( !at_mode_keyword() )
    {
        return parse_type();
    }

    const Keyword mode = peek().keyword();
    advance();

    // D32 gives one order. Without this, `ref const T` falls through to parse_type's leading-const
    // branch and becomes Mode_type( Const_type( T ) ) - an inner const, which the checker reports as
    // "a pointer to `const` is not supported yet": the right refusal for the wrong reason.
    if( check_keyword( Keyword::Const ) )
    {
        error_at(
            peek().span, "`const` comes before the mode", fmt::format( "write `const {} T`", sm_.text( previous().span ) )
        );

        advance();
    }

    Node_id type =
        ast_.add( Node_kind::Mode_type, Span::merge( start, previous().span ), static_cast<u32>( mode ), { parse_type() } );

    if( leading_const )
    {
        type = ast_.add( Node_kind::Const_type, Span::merge( start, previous().span ), 0, { type } );
    }

    return type;
}

Node_id Parser::parse_param()
{
    const Span start = peek().span;

    // D31: the marker appears in the signature as well as at the call site, and the two must agree.
    // Wrapped around the type rather than stored on the Param_decl, because aux already holds the
    // name and this is the shape Ref_type had - one node in type position, generalised.
    Node_id type = parse_type_with_mode();

    // Same convention as Function_decl: the name is a token, so it goes in aux rather than becoming
    // a second child.
    const Symbol_id name = expect_name();

    // A declaration with no name is not one: sema reads every declaration's name to report
    // about it, so handing one over means a keyword or a missing identifier crashes a pass
    // that had no reason to expect it. Everything inside was still parsed, so the errors in
    // there are already reported.
    if( !name.is_valid() )
    {
        return error_node( Span::merge( start, previous().span ) );
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

bool Parser::peek_is_adjacent() const
{
    return pos_ > 0 && peek().span.file == previous().span.file && peek().span.start == previous().span.end;
}

bool Parser::match_generic_close()
{
    if( pending_greater_ > 0 )
    {
        pending_greater_ -= 1;
        return true;
    }

    if( match( Token_kind::Greater ) )
    {
        return true;
    }

    if( check( Token_kind::Greater_greater ) )
    {
        advance();
        pending_greater_ = 1;
        return true;
    }

    return false;
}

bool Parser::can_start_expression() const
{
    switch( peek().kind )
    {
    case Token_kind::Identifier:
    case Token_kind::Int_literal:
    case Token_kind::Float_literal:
    case Token_kind::String_literal:
    case Token_kind::Char_literal:
    case Token_kind::L_paren:
        return true;

    // Unary operators. parse_prefix does not handle all of them yet, but `*p = 1;` is a
    // statement, so the guard has to let them through.
    case Token_kind::Minus:
    case Token_kind::Bang:
    case Token_kind::Tilde:
    case Token_kind::Star:
    case Token_kind::Amp:
        return true;

    // Must agree with parse_prefix's Keyword case, which is the other half of this list. Three
    // features in a row have needed both edited together; a keyword missing from here parses fine
    // in an argument or an initialiser and fails only in statement position.
    case Token_kind::Keyword:
        return check_keyword( Keyword::True ) || check_keyword( Keyword::False ) || check_keyword( Keyword::Nullptr ) ||
               check_keyword( Keyword::Move ) || check_keyword( Keyword::Out ) || check_keyword( Keyword::Ref ) ||
               check_keyword( Keyword::Cast ) || check_keyword( Keyword::Wrap ) || check_keyword( Keyword::This );

    default:
        return false;
    }
}

bool Parser::looks_like_declaration()
{
    // `auto x = ...` is settled by its keyword; the caller checks that before asking.
    if( !check( Token_kind::Identifier ) && !check_keyword( Keyword::Const ) )
    {
        return false;
    }

    const u32  saved = pos_;
    const bool found = scan_type_and_name();

    pos_ = saved;
    return found;
}

bool Parser::looks_like_binding()
{
    const u32 saved = pos_;

    // `const ref T r` - the const leads, so it is stepped over before the mode is looked for.
    // Restored on every path, so a `const i32 x` that is not a binding still reaches
    // looks_like_declaration exactly as it did.
    match_keyword( Keyword::Const );

    if( !at_mode_keyword() )
    {
        pos_ = saved;
        return false;
    }

    advance(); // the mode

    const bool found = scan_type_and_name();

    pos_ = saved;
    return found;
}

// Leaves pos_ just after the declared name when it finds one, and restores it otherwise - so a
// caller wanting only the answer can ignore the cursor, and one wanting to see what follows the
// name restores it itself.
bool Parser::scan_type_and_name()
{
    const u32 saved = pos_;

    // A type may open with const: `const i32 x = 0;`.
    while( match_keyword( Keyword::Const ) )
    {
    }

    // ...and then with a mode: `const ref T r`, `ref T r`. Here rather than only in
    // looks_like_binding because parse_declaration scans with this too, and a `const ref` return
    // type has to be recognised as a function before its `(` is ever reached.
    if( at_mode_keyword() )
    {
        advance();
    }

    if( !match( Token_kind::Identifier ) )
    {
        pos_ = saved;
        return false;
    }

    // Whatever a type can be followed by before the declared name: `Point*`, `const T&`,
    // `Vector<i32>`. Anything else means this was an expression after all.
    while( true )
    {
        // D17 again: only an adjacent `*`/`&` is part of a type, so `a * b;` is not a declaration
        // and falls through to being an expression statement, where D15 rejects it.
        if( ( check( Token_kind::Star ) || check( Token_kind::Amp ) ) && peek_is_adjacent() )
        {
            advance();
            continue;
        }

        if( match_keyword( Keyword::Const ) )
        {
            continue;
        }

        // A generic argument list. Nesting is counted rather than recursed, because this is only a
        // scan - `Vector<Vector<i32>>` closes with `>>`, one token carrying two levels.
        if( check( Token_kind::Less ) )
        {
            u32 depth = 0;

            while( !at_end() )
            {
                if( match( Token_kind::Less ) )
                {
                    depth += 1;
                }
                else if( match( Token_kind::Greater ) )
                {
                    depth -= 1;
                }
                else if( match( Token_kind::Greater_greater ) )
                {
                    depth = depth >= 2 ? depth - 2 : 0;
                }
                else if( check( Token_kind::Semicolon ) || check( Token_kind::R_brace ) )
                {
                    break; // unbalanced - not a type
                }
                else
                {
                    advance();
                }

                if( depth == 0 )
                {
                    break;
                }
            }

            if( depth != 0 )
            {
                pos_ = saved;
                return false;
            }

            continue;
        }

        break;
    }

    // A type is only a declaration if a name follows it. `a * b;` scans a type-shaped `a *` and
    // then finds `b`, which is why D15 has to make the discarded-multiply reading illegal - see
    // PLAN §6.3 D15 and L17.
    // A keyword here is a name that cannot be one - `i32 out = 1;`. Nothing valid has a type
    // followed by a keyword, so taking the declaration path costs nothing and lets parse_var_decl
    // report the real problem rather than a stray `;`.
    if( !check( Token_kind::Identifier ) && !check( Token_kind::Keyword ) )
    {
        pos_ = saved;
        return false;
    }

    advance(); // past the name, so a caller can see what comes after it
    return true;
}

Node_id Parser::parse_statement()
{
    if( check_keyword( Keyword::Return ) )
    {
        return parse_return_stmt();
    }

    if( check_keyword( Keyword::Break ) )
    {
        const Span start = peek().span;
        advance();
        expect( Token_kind::Semicolon );
        return ast_.add( Node_kind::Break_stmt, Span::merge( start, previous().span ), 0, {} );
    }

    if( check_keyword( Keyword::Continue ) )
    {
        const Span start = peek().span;
        advance();
        expect( Token_kind::Semicolon );
        return ast_.add( Node_kind::Continue_stmt, Span::merge( start, previous().span ), 0, {} );
    }

    if( check( Token_kind::L_brace ) )
    {
        return parse_block();
    }

    if( check_keyword( Keyword::If ) )
    {
        return parse_if_stmt();
    }

    if( check_keyword( Keyword::While ) )
    {
        return parse_while_stmt();
    }

    if( check_keyword( Keyword::For ) )
    {
        return parse_for_stmt();
    }

    // A struct in a function body. Recognised here so it can be consumed whole: synchronise()
    // would stop at the first `;` *inside* the struct, and everything after it would then be read
    // as a top-level declaration - one mistake becoming five.
    if( check_keyword( Keyword::Struct ) || check_keyword( Keyword::Class ) )
    {
        const Span start = peek().span;

        error_at(
            start,
            "a struct or a class cannot be declared inside a function",
            "declare it at file scope, where it is visible throughout the file"
        );

        // Parsed for its cursor movement, not its result: the node is not a statement, so an
        // Error node stands in its place. A malformed struct still reports from in there.
        parse_aggregate_decl();

        return error_node( Span::merge( start, previous().span ) );
    }

    // The keyword test is free; looks_like_declaration() moves the cursor and puts it back.
    if( check_keyword( Keyword::Auto ) || looks_like_binding() || looks_like_declaration() )
    {
        return parse_var_decl();
    }

    // Anything that can begin an expression becomes an expression statement; D15 then decides
    // whether it is one with an effect.
    if( can_start_expression() )
    {
        return parse_expression_stmt();
    }

    // Statements are the recovery boundary: this is the one place synchronise() belongs, because
    // its stop set - `;`, `}`, statement keywords - is exactly the set of statement boundaries.
    const Span span = peek().span;

    // A poisoned token: the lexer already reported it, so a second message here would be noise.
    if( !check( Token_kind::Unknown ) )
    {
        error_at( span, fmt::format( "expected a statement, found {}", found_text() ) );
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

Node_id Parser::parse_var_decl()
{
    const Span start = peek().span;

    Node_id type;
    if( check_keyword( Keyword::Auto ) )
    {
        advance();         // consume auto
        type = Node_id {}; // placeholder for type to be inferred later
    }
    else
    {
        type = parse_type_with_mode();
    }

    const Symbol_id name = expect_name();

    Node_id value;
    if( match( Token_kind::Equal ) )
    {
        value = parse_expression( 0 );
    }

    expect( Token_kind::Semicolon );
    // A declaration with no name is not one: sema reads every declaration's name to report
    // about it, so handing one over means a keyword or a missing identifier crashes a pass
    // that had no reason to expect it. Everything inside was still parsed, so the errors in
    // there are already reported.
    if( !name.is_valid() )
    {
        return error_node( Span::merge( start, previous().span ) );
    }

    return ast_.add( Node_kind::Var_decl, Span::merge( start, previous().span ), name.v, { type, value } );
}

Node_id Parser::parse_expression_stmt( bool consume_semicolon )
{
    const Span    start = peek().span;
    const Node_id expr  = parse_expression( 0 );

    Node_kind            kind     = Node_kind::Expr_stmt;
    u32                  aux      = 0;
    std::vector<Node_id> children = { expr };

    if( is_assignment( peek().kind ) )
    {
        Token op = advance();
        children.push_back( parse_expression( 0 ) );
        kind = Node_kind::Assign_stmt;
        aux  = static_cast<u32>( op.kind );
    }
    else if( check( Token_kind::Plus_plus ) || check( Token_kind::Minus_minus ) )
    {
        Token op = advance();
        kind     = Node_kind::Increment_stmt;
        aux      = static_cast<u32>( op.kind );
    }
    else if( ast_.kind( expr ) != Node_kind::Call_expr && ast_.kind( expr ) != Node_kind::Error )
    {
        // `u32 *ptr;` and `u32 &r;` are what a C++ programmer writes from habit, and neither reaches
        // the type parser: the first is a multiplication under D17, the second a bitwise and now
        // that D32 has taken `&` out of type position. Without this both land on the generic "no
        // effect" message, which explains nothing.
        const Token_kind op =
            ast_.kind( expr ) == Node_kind::Binary_expr ? static_cast<Token_kind>( ast_.aux( expr ) ) : Token_kind::Unknown;

        const bool looks_like_a_declaration = ( op == Token_kind::Star || op == Token_kind::Amp ) &&
                                              ast_.kind( ast_.children( expr )[0] ) == Node_kind::Name_expr &&
                                              ast_.kind( ast_.children( expr )[1] ) == Node_kind::Name_expr;

        if( looks_like_a_declaration )
        {
            const std::string_view type = sm_.text( ast_.span( ast_.children( expr )[0] ) );
            const std::string_view name = sm_.text( ast_.span( ast_.children( expr )[1] ) );

            // Different rules, so different messages: `*` is a spacing mistake, `&` is a spelling
            // that no longer exists.
            if( op == Token_kind::Star )
            {
                error_at(
                    ast_.span( expr ),
                    "`*` must touch the type it modifies",
                    fmt::format( "write `{}* {}` to declare a pointer", type, name )
                );
            }
            else
            {
                error_at(
                    ast_.span( expr ),
                    "`&` is not a type",
                    fmt::format( "write `ref {} {}` for a reference binding", type, name )
                );
            }
        }
        else
        {
            error_at( ast_.span( expr ), "this expression has no effect", "assign the result, or remove it" );
        }
    }

    if( consume_semicolon )
    {
        expect( Token_kind::Semicolon );
    }

    return ast_.add( kind, Span::merge( start, previous().span ), aux, children );
}

Node_id Parser::parse_if_stmt()
{
    assert( check_keyword( Keyword::If ) );

    const Span start = peek().span;
    advance();

    expect( Token_kind::L_paren );
    const Node_id condition = parse_expression( 0 );
    expect( Token_kind::R_paren );

    const Node_id then_branch = parse_block();

    Node_id else_branch; // invalid when absent
    if( match_keyword( Keyword::Else ) )
    {
        // The one exception to D3: `else if` chains rather than demanding braces.
        else_branch = check_keyword( Keyword::If ) ? parse_if_stmt() : parse_block();
    }

    return ast_.add( Node_kind::If_stmt, Span::merge( start, previous().span ), 0, { condition, then_branch, else_branch } );
}

Node_id Parser::parse_while_stmt()
{
    assert( check_keyword( Keyword::While ) );

    const Span start = peek().span;
    advance();
    expect( Token_kind::L_paren );
    const Node_id condition = parse_expression( 0 );
    expect( Token_kind::R_paren );
    const Node_id body = parse_block();
    return ast_.add( Node_kind::While_stmt, Span::merge( start, previous().span ), 0, { condition, body } );
}

Node_id Parser::parse_for_stmt()
{
    assert( check_keyword( Keyword::For ) );

    const Span start = peek().span;
    advance();
    expect( Token_kind::L_paren );

    // Init. Both parse_var_decl and parse_expression_stmt consume their own `;` which is exactly
    // the first semicolon of the header - so neither needs changing
    Node_id init;
    if( !match( Token_kind::Semicolon ) )
    {
        if( check_keyword( Keyword::Auto ) || looks_like_binding() || looks_like_declaration() )
        {
            init = parse_var_decl();
        }
        else
        {
            init = parse_expression_stmt();
        }
    }

    // Condition. An expression, then the second `;`.
    Node_id condition;
    if( !check( Token_kind::Semicolon ) )
    {
        condition = parse_expression( 0 );
    }
    expect( Token_kind::Semicolon );

    // Update. Followed by `)` rather than `;`, which is the one place the statement rules cannot
    // be reused unchanged - hence the flag. Assignment, increment and D15 all still apply.
    Node_id update;
    if( !check( Token_kind::R_paren ) )
    {
        update = parse_expression_stmt( false );
    }

    expect( Token_kind::R_paren );
    const Node_id body = parse_block();

    return ast_.add( Node_kind::For_stmt, Span::merge( start, previous().span ), 0, { init, condition, update, body } );
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

Node_id Parser::synthesise_receiver( Symbol_id enclosing, Span span )
{
    return ast_.add(
        Node_kind::Param_decl,
        span,
        Interner::keyword( Keyword::This ).v,
        { ast_.add( Node_kind::Pointer_type, span, 0, { ast_.add( Node_kind::Named_type, span, enclosing.v, {} ) } ) }
    );
}

bool Parser::at_mode_keyword() const
{
    return check_keyword( Keyword::Move ) || check_keyword( Keyword::Ref ) || check_keyword( Keyword::Out );
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
        if( check( Token_kind::L_paren ) )
        {
            Node_id args = parse_arg_list();

            left = ast_.add( Node_kind::Call_expr, Span::merge( ast_.span( left ), ast_.span( args ) ), 0, { left, args } );
            continue;
        }

        // Field access binds tightest, like a call, and for the same reason produces its own node
        // rather than a Binary_expr. The field name goes in aux, not into a Name_expr child: it
        // resolves against the object's type, not through the scope stack, and a child would
        // invite the resolver to look for a variable called `x` in `p.x`.
        // D22: `.` reaches through a pointer, so `->` has no work left to do. It is rejected by
        // name rather than left to lex as `-` then `>`, which would produce a message about
        // arithmetic - and then recovered *as* the field access it was meant to be. Reporting
        // without consuming would drop it into the infix path below and build a Binary_expr whose
        // operator is `->`, which sema would have to reject all over again.
        const bool arrow = check( Token_kind::Arrow );

        if( arrow || check( Token_kind::Dot ) )
        {
            if( arrow )
            {
                error_at( peek().span, "`->` is not a Keel operator", "use `.`, which reaches through a pointer" );
            }

            advance();

            // Only on success: a failed expect() does not advance, so previous() would be the dot
            // itself and its symbol would become the field's name.
            Symbol_id name;
            if( expect( Token_kind::Identifier ) )
            {
                name = previous().symbol;
            }

            left = ast_.add( Node_kind::Field_expr, Span::merge( ast_.span( left ), previous().span ), name.v, { left } );
            continue;
        }

        if( check( Token_kind::Colon_colon ) )
        {
            advance();

            // Only on success: a failed expect() does not advance, so previous() would be the `::`
            // itself and its symbol would become the field's name.
            Symbol_id name;
            if( expect( Token_kind::Identifier ) )
            {
                name = previous().symbol;
            }

            left = ast_.add( Node_kind::Path_expr, Span::merge( ast_.span( left ), previous().span ), name.v, { left } );
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

// The keyword-headed expressions: literals with no digits, the three call-site markers, and the
// two conversions. Split out because it is the only case here with real branching - the rest of
// parse_prefix is one line per token kind.
Node_id Parser::parse_keyword_prefix( Span start )
{
    if( check_keyword( Keyword::True ) || check_keyword( Keyword::False ) )
    {
        const bool value = check_keyword( Keyword::True );
        advance();
        return ast_.add( Node_kind::Bool_literal, Span::merge( start, previous().span ), value ? 1u : 0u, {} );
    }

    if( check_keyword( Keyword::Nullptr ) )
    {
        advance();
        return ast_.add( Node_kind::Null_literal, Span::merge( start, previous().span ), 0, {} );
    }

    if( check_keyword( Keyword::Cast ) || check_keyword( Keyword::Wrap ) )
    {
        // aux carries which of the two, the way Marker_expr does: one node kind, and sema
        // reads the operator back off it.
        const Keyword which = peek().keyword();
        advance();

        expect( Token_kind::Less );
        const Node_id type = parse_type();

        // Not expect( Greater ): `wrap<Vector<i32>>( x )` closes with `>>`, and this is the
        // helper that splits it.
        if( !match_generic_close() )
        {
            error_expected( Token_kind::Greater );
        }

        expect( Token_kind::L_paren );
        const Node_id operand = parse_expression( 0 );
        expect( Token_kind::R_paren );

        return ast_.add(
            Node_kind::Cast_expr, Span::merge( start, previous().span ), static_cast<u32>( which ), { type, operand }
        );
    }

    if( check_keyword( Keyword::Move ) || check_keyword( Keyword::Out ) || check_keyword( Keyword::Ref ) )
    {
        const Keyword marker = peek().keyword();
        advance();
        const Node_id operand = parse_expression( 0 );
        return ast_.add(
            Node_kind::Marker_expr, Span::merge( start, previous().span ), static_cast<u32>( marker ), { operand }
        );
    }

    if( check_keyword( Keyword::This ) )
    {
        advance();
        return ast_.add( Node_kind::Name_expr, previous().span, Interner::keyword( Keyword::This ).v, {} );
    }

    // Not a keyword that starts an expression. Invalid rather than a diagnostic, so the caller
    // reports it on the same path an unknown token takes - one message, in one place.
    return Node_id {};
}

Node_id Parser::parse_prefix()
{
    const Span start = peek().span;

    switch( peek().kind )
    {
    case Token_kind::Int_literal:
        advance();
        return ast_.add( Node_kind::Int_literal, Span::merge( start, previous().span ), previous().symbol.v, {} );

    case Token_kind::Float_literal:
        advance();
        return ast_.add( Node_kind::Float_literal, Span::merge( start, previous().span ), previous().symbol.v, {} );

    case Token_kind::String_literal:
        advance();
        return ast_.add( Node_kind::String_literal, Span::merge( start, previous().span ), 0, {} );

    case Token_kind::Char_literal:
        advance();
        return ast_.add( Node_kind::Char_literal, Span::merge( start, previous().span ), previous().symbol.v, {} );

    case Token_kind::Identifier:
    {
        advance();

        const Symbol_id name = previous().symbol;

        // No ambiguity with a block: a block is a statement and starts with `{`, so an expression
        // is never followed by one. Rust needs a rule here only because its `if` takes no
        // parentheses - Keel's conditions end at `)`.
        if( check( Token_kind::L_brace ) )
        {
            return parse_struct_literal( start, name );
        }

        return ast_.add( Node_kind::Name_expr, Span::merge( start, previous().span ), name.v, {} );
    }

    case Token_kind::Minus:
    case Token_kind::Bang:
    case Token_kind::Tilde:
    case Token_kind::Star:
    case Token_kind::Amp:
    {
        // The operator is captured before the operand is parsed, and the span read after it.
        const Token op = advance();

        // Recursing at the same power makes unary right-associative, so `- -x` nests. No enclosing
        // operator is passed: unary is not in D16's classification, and claiming it were would make
        // `-a << b` a spurious "cannot be mixed" error.
        const Node_id operand = parse_expression( k_unary_power );

        return ast_.add(
            Node_kind::Unary_expr, Span::merge( start, previous().span ), static_cast<u32>( op.kind ), { operand }
        );
    }

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
    {
        const Node_id keyword_expression = parse_keyword_prefix( start );

        if( keyword_expression.is_valid() )
        {
            return keyword_expression;
        }

        break; // an unrecognised keyword falls through to the message below
    }
    default:
        break;
    }

    // The lexer already reported an Unknown token; do not report it twice.
    if( !check( Token_kind::Unknown ) )
    {
        error_at( peek().span, fmt::format( "expected an expression, found {}", found_text() ) );
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
        ast_  = parse( lex( file_, sm_, interner_, literals_, diags_ ), sm_, diags_ );
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
    Literals       literals_;
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
    if( p.kind( id ) == Node_kind::Unary_expr )
    {
        return std::string( token_kind_spelling( static_cast<Token_kind>( p.aux( id ) ) ) ) + "(" +
               shape( p, p.child( id, 0 ) ) + ")";
    }

    if( p.kind( id ) == Node_kind::Call_expr )
    {
        return "call(" + shape( p, p.child( id, 0 ) ) + "," + shape( p, p.child( id, 1 ) ) + ")";
    }

    if( p.kind( id ) == Node_kind::Pointer_type )
    {
        return shape( p, p.child( id, 0 ) ) + "*";
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

// The statement of a one-statement function body.
Node_id first_statement( const Parsed& p )
{
    const Node_id block = find_first( p.ast(), p.root(), Node_kind::Block );
    return block.is_valid() && !p.children( block ).empty() ? p.child( block, 0 ) : Node_id {};
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

// D18 removed the need for forward declarations, so a `;` where a body belongs is a mistake a C++
// author makes out of habit. It used to cascade: parse_block carried on and read the declarations
// that followed as statements.
// PLAN §6.2: `struct Point { f64 x; f64 y; };` - C++'s spelling, trailing semicolon included.
// PLAN §6.2: `Point { 0.0, 0.0 }`. No ambiguity with a block - Keel's conditions end at `)`, and a
// block is a statement, so an expression is never directly followed by one.
TEST_CASE( "parser_parses_struct_literals", "[parse]" )
{
    SECTION( "positional initialisers carry no name" )
    {
        const Parsed p( "i32 main() { auto q = Point { 1.0, 2.0 }; return 0; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id literal = find_first( p.ast(), p.root(), Node_kind::Struct_literal );

        REQUIRE( literal.is_valid() );
        REQUIRE( Symbol_id { p.aux( literal ) }.is_valid() ); // the type name
        REQUIRE( p.children( literal ).size() == 2 );

        for( const Node_id init : p.children( literal ) )
        {
            REQUIRE( p.kind( init ) == Node_kind::Field_init );
            REQUIRE_FALSE( Symbol_id { p.aux( init ) }.is_valid() ); // positional: no field name
            REQUIRE( p.children( init ).size() == 1 );
        }
    }

    SECTION( "designated initialisers carry one" )
    {
        const Parsed p( "i32 main() { auto q = Point { .x = 1.0, .y = 2.0 }; return 0; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id literal = find_first( p.ast(), p.root(), Node_kind::Struct_literal );

        REQUIRE( p.children( literal ).size() == 2 );
        REQUIRE( Symbol_id { p.aux( p.child( literal, 0 ) ) }.is_valid() );
        REQUIRE( Symbol_id { p.aux( p.child( literal, 1 ) ) }.is_valid() );
        REQUIRE( p.aux( p.child( literal, 0 ) ) != p.aux( p.child( literal, 1 ) ) );
    }

    // C++ permits a trailing comma in a braced initialiser and rejects one in a call. §5.1 says to
    // follow that rather than be internally consistent with parse_arg_list.
    SECTION( "a trailing comma is allowed, unlike in an argument list" )
    {
        const Parsed p( "i32 main() { auto q = Point { 1.0, 2.0, }; return 0; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( find_first( p.ast(), p.root(), Node_kind::Struct_literal ) ).size() == 2 );
    }

    SECTION( "an empty literal" )
    {
        const Parsed p( "i32 main() { auto q = Empty { }; return 0; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( find_first( p.ast(), p.root(), Node_kind::Struct_literal ) ).empty() );
    }

    SECTION( "literals nest" )
    {
        const Parsed p( "i32 main() { auto q = Line { Point { 1.0, 2.0 }, Point { 3.0, 4.0 } }; return 0; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id outer = find_first( p.ast(), p.root(), Node_kind::Struct_literal );
        const Node_id inner = p.child( p.child( outer, 0 ), 0 );

        REQUIRE( p.kind( inner ) == Node_kind::Struct_literal );
        REQUIRE( p.children( inner ).size() == 2 );
    }

    // An identifier not followed by `{` is still just a name - the literal path must not swallow
    // ordinary expressions.
    SECTION( "a bare identifier is still a name" )
    {
        const Parsed p( "i32 main() { i32 y = 1; return y; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE_FALSE( find_first( p.ast(), p.root(), Node_kind::Struct_literal ).is_valid() );
    }

    SECTION( "malformed initialisers report" )
    {
        for( const char* source : {
                 "i32 main() { auto q = Point { . = 1.0 }; return 0; }", // no field name
                 "i32 main() { auto q = Point { .x 1.0 }; return 0; }",  // no `=`
                 "i32 main() { auto q = Point { 1.0 2.0 }; return 0; }", // no comma
                 "i32 main() { auto q = Point { 1.0, ; return 0; }",     // unterminated
             } )
        {
            const Parsed p( source );

            INFO( "source: " << source << "\n" << p.errors() );
            REQUIRE( p.has_errors() );
        }
    }
}

// `move x`, `out x`, `ref x`. One node kind: they are syntactically identical, and aux carries
// which one, the way Binary_expr carries its operator.
// `move`, `out` and `ref` are ordinary identifiers in C++, so a program can arrive here without
// its author suspecting the *name* is the problem.
TEST_CASE( "parser_explains_a_keyword_used_as_a_name", "[parse]" )
{
    static const char* sources[] = {
        "i32 ref() { return 0; }",               // a function
        "struct move { i32 x; };",               // a struct
        "struct S { i32 move; };",               // a field
        "i32 f( i32 ref ) { return 0; }",        // a parameter
        "i32 main() { i32 out = 1; return 0; }", // a local
    };

    for( const char* source : sources )
    {
        const Parsed p( source );

        INFO( "source: " << source << "\n" << p.errors() );

        // One message per mistake: the keyword is consumed as the name it was meant to be, so it
        // does not go on to trip every rule after it.
        REQUIRE( p.error_count() == 1 );
        REQUIRE( p.errors().find( "is a keyword, so it cannot be used as a name" ) != std::string::npos );
    }
}

// Sema reads every declaration's name to report about it, so one without a name must not reach it.
// `i32 () { return 0; }` used to abort inside Interner::text.
TEST_CASE( "parser_makes_a_nameless_declaration_an_error_node", "[parse]" )
{
    static const char* sources[] = {
        "i32 () { return 0; }",
        "struct { i32 x; };",
        "struct S { i32 ; };",
        "i32 main() { auto = 5; return 0; }",
    };

    for( const char* source : sources )
    {
        const Parsed p( source );

        INFO( "source: " << source << "\n" << p.errors() );
        REQUIRE( p.has_errors() );

        // Nothing anywhere in the tree claims to be a declaration without a name.
        for( u32 i = 0; i < p.ast().node_count(); ++i )
        {
            const Node_id   id   = Node_id { i };
            const Node_kind kind = p.kind( id );

            const bool declares_a_name = kind == Node_kind::Function_decl || kind == Node_kind::Struct_decl ||
                                         kind == Node_kind::Var_decl || kind == Node_kind::Field_decl ||
                                         kind == Node_kind::Param_decl;

            if( declares_a_name )
            {
                INFO( "node " << i );
                REQUIRE( Symbol_id { p.aux( id ) }.is_valid() );
            }
        }
    }
}

TEST_CASE( "parser_parses_passing_markers", "[parse]" )
{
    struct Case
    {
        std::string_view source;
        Keyword          marker;
    };

    static const Case cases[] = {
        { "i32 main() { return f( move b ); }", Keyword::Move },
        { "i32 main() { return f( out b ); }", Keyword::Out },
        { "i32 main() { return f( ref b ); }", Keyword::Ref },
    };

    for( const Case& c : cases )
    {
        const Parsed p( c.source );

        INFO( "source: " << c.source << "\n" << p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id marker = find_first( p.ast(), p.root(), Node_kind::Marker_expr );

        REQUIRE( marker.is_valid() );
        REQUIRE( p.aux( marker ) == static_cast<u32>( c.marker ) );
        REQUIRE( p.children( marker ).size() == 1 );
        REQUIRE( p.kind( p.child( marker, 0 ) ) == Node_kind::Name_expr );
    }
}

// A marker annotates a whole argument rather than binding as an operator, so it takes the
// expression up to the boundary. Under unary binding `f( move a + b )` would be `( move a ) + b`,
// which passes the *sum* by copy - leaving a `move` at the call site saying nothing about what the
// callee receives, which is the one thing D2 exists to guarantee. Whether the operand is a place
// is sema's question, not the grammar's.
TEST_CASE( "parser_binds_a_passing_marker_to_the_whole_argument", "[parse]" )
{
    SECTION( "field access is part of it" )
    {
        const Parsed p( "i32 main() { return f( move a.b ); }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id marker = find_first( p.ast(), p.root(), Node_kind::Marker_expr );
        REQUIRE( p.kind( p.child( marker, 0 ) ) == Node_kind::Field_expr );
    }

    // The marker contains the sum rather than being its left operand. Sema then rejects it, since
    // a sum is not a place - which is the whole diagnostic this parse buys.
    SECTION( "so is arithmetic" )
    {
        const Parsed p( "i32 main() { return f( move a + b ); }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id marker = find_first( p.ast(), p.root(), Node_kind::Marker_expr );

        REQUIRE( marker.is_valid() );
        REQUIRE( p.kind( p.child( marker, 0 ) ) == Node_kind::Binary_expr );
    }

    // Parentheses are how the other reading is written, and it has to be written.
    SECTION( "parentheses give back the operand reading" )
    {
        const Parsed p( "i32 main() { return f( ( move a ) + b ); }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id sum = find_first( p.ast(), p.root(), Node_kind::Binary_expr );

        REQUIRE( sum.is_valid() );
        REQUIRE( p.kind( p.child( sum, 0 ) ) == Node_kind::Marker_expr );
    }
}

// `move` is legal wherever a value is - D31 governs initialisation and assignment as well as
// arguments - so restricting it to argument lists is not the grammar's job.
TEST_CASE( "parser_accepts_markers_outside_argument_lists", "[parse]" )
{
    for( const char* source : {
             "i32 main() { i32 c = move b; return 0; }",
             "i32 main() { return move b; }",
             "i32 main() { b = move c; return 0; }",
         } )
    {
        const Parsed p( source );

        INFO( "source: " << source << "\n" << p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( find_first( p.ast(), p.root(), Node_kind::Marker_expr ).is_valid() );
    }
}

// can_start_expression() gates statement position only - argument lists and initialisers reach
// parse_prefix directly. Without the markers listed there, `move b;` is "expected a statement,
// found `move`" rather than D15 explaining that the statement does nothing.
TEST_CASE( "parser_reaches_a_marker_in_statement_position", "[parse]" )
{
    const Parsed p( "i32 main() { move b; return 0; }" );

    INFO( p.errors() );

    REQUIRE( find_first( p.ast(), p.root(), Node_kind::Marker_expr ).is_valid() );
    REQUIRE( p.errors().find( "no effect" ) != std::string::npos );
    REQUIRE( p.errors().find( "expected a statement" ) == std::string::npos );
}

TEST_CASE( "parser_reports_a_marker_with_no_operand", "[parse]" )
{
    for( const char* source : { "i32 main() { return f( move ); }", "i32 main() { return move; }" } )
    {
        const Parsed p( source );

        INFO( "source: " << source << "\n" << p.errors() );
        REQUIRE( p.has_errors() );
    }
}

TEST_CASE( "parser_parses_field_access", "[parse]" )
{
    SECTION( "the name goes in aux, not into a Name_expr child" )
    {
        const Parsed p( "i32 main() { return p.x; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id field = find_first( p.ast(), p.root(), Node_kind::Field_expr );

        REQUIRE( field.is_valid() );
        REQUIRE( p.children( field ).size() == 1 );
        REQUIRE( p.kind( p.child( field, 0 ) ) == Node_kind::Name_expr ); // the object
        REQUIRE( Symbol_id { p.aux( field ) }.is_valid() );               // the field's symbol
        REQUIRE( p.text( field ) == "p.x" );
    }

    SECTION( "access chains" )
    {
        const Parsed p( "i32 main() { return p.x.y; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id outer = find_first( p.ast(), p.root(), Node_kind::Field_expr );
        REQUIRE( p.kind( p.child( outer, 0 ) ) == Node_kind::Field_expr );
    }

    SECTION( "and applies to whatever precedes it" )
    {
        const Parsed p( "i32 main() { return f().x; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id field = find_first( p.ast(), p.root(), Node_kind::Field_expr );
        REQUIRE( p.kind( p.child( field, 0 ) ) == Node_kind::Call_expr );
    }

    // Tighter than every operator, so this is `(a.x) + (b.y)` and not `a.(x + b).y`.
    SECTION( "it binds tighter than arithmetic" )
    {
        const Parsed p( "i32 main() { return a.x + b.y; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id root_expr = find_first( p.ast(), p.root(), Node_kind::Binary_expr );

        REQUIRE( root_expr.is_valid() );
        REQUIRE( p.kind( p.child( root_expr, 0 ) ) == Node_kind::Field_expr );
        REQUIRE( p.kind( p.child( root_expr, 1 ) ) == Node_kind::Field_expr );
    }

    SECTION( "a field name is required after the dot" )
    {
        for( const char* source : { "i32 main() { return p.; }", "i32 main() { return p.5; }" } )
        {
            const Parsed p( source );

            INFO( "source: " << source << "\n" << p.errors() );
            REQUIRE( p.has_errors() );
            REQUIRE( p.errors().find( "expected an identifier" ) != std::string::npos );
        }
    }
}

// D22. `->` is lexed rather than deleted precisely so it can be rejected by name: removing the
// token would make `p->x` lex as `-` then `>` and produce a message about arithmetic.
TEST_CASE( "parser_rejects_the_arrow_operator", "[parse]" )
{
    const Parsed p( "i32 main() { return p->x; }" );

    INFO( p.errors() );
    REQUIRE( p.error_count() == 1 );
    REQUIRE( p.errors().find( "`->` is not a Keel operator" ) != std::string::npos );
    REQUIRE( p.errors().find( "use `.`" ) != std::string::npos );

    // Recovery treats it as the field access it was meant to be, so nothing after it cascades.
    SECTION( "and the access still parses" )
    {
        const Node_id field = find_first( p.ast(), p.root(), Node_kind::Field_expr );

        REQUIRE( field.is_valid() );
        REQUIRE( p.text( field ) == "p->x" );
    }

    SECTION( "a chain reports once per arrow, not once per token" )
    {
        const Parsed chained( "i32 main() { return a->b->c; }" );

        INFO( chained.errors() );
        REQUIRE( chained.error_count() == 2 );
    }
}

TEST_CASE( "parser_parses_a_struct_declaration", "[parse]" )
{
    const Parsed p( "struct Point\n{\n    f64 x;\n    f64 y;\n};\n" );

    INFO( p.errors() );
    REQUIRE_FALSE( p.has_errors() );

    const Node_id decl = p.child( p.root(), 0 );
    REQUIRE( p.kind( decl ) == Node_kind::Struct_decl );
    REQUIRE( p.text( decl ).starts_with( "struct Point" ) );

    // Fields are the children, in declaration order - layout is declaration order (§9, M2).
    REQUIRE( p.children( decl ).size() == 2 );
    REQUIRE( p.kind( p.child( decl, 0 ) ) == Node_kind::Field_decl );
    REQUIRE( p.kind( p.child( decl, 1 ) ) == Node_kind::Field_decl );

    // A field carries its name in aux and its type as its one child, exactly like Param_decl.
    const Node_id first = p.child( decl, 0 );
    REQUIRE( p.children( first ).size() == 1 );
    REQUIRE( p.kind( p.child( first, 0 ) ) == Node_kind::Named_type );
    REQUIRE( p.text( p.child( first, 0 ) ) == "f64" );
    REQUIRE( p.aux( first ) != p.aux( p.child( decl, 1 ) ) ); // x and y are different symbols
}

TEST_CASE( "parser_parses_struct_edge_cases", "[parse]" )
{
    SECTION( "a struct with no fields" )
    {
        const Parsed p( "struct Empty { };" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.kind( p.child( p.root(), 0 ) ) == Node_kind::Struct_decl );
        REQUIRE( p.children( p.child( p.root(), 0 ) ).empty() );
    }

    SECTION( "one field" )
    {
        const Parsed p( "struct Wrapper { i32 value; };" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( p.child( p.root(), 0 ) ).size() == 1 );
    }

    SECTION( "a field whose type is another struct" )
    {
        const Parsed p( "struct Point { f64 x; };\nstruct Line { Point a; Point b; };\n" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( p.root() ).size() == 2 );
        REQUIRE( p.children( p.child( p.root(), 1 ) ).size() == 2 );
    }

    SECTION( "a const field type" )
    {
        const Parsed p( "struct Fixed { const i32 value; };" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.kind( p.child( p.child( p.child( p.root(), 0 ), 0 ), 0 ) ) == Node_kind::Const_type );
    }

    SECTION( "a pointer field - what M3's Buffer needs" )
    {
        const Parsed p( "struct Buffer { u8* ptr; u64 len; };" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id field = p.child( p.child( p.root(), 0 ), 0 );
        REQUIRE( p.kind( p.child( field, 0 ) ) == Node_kind::Pointer_type );
    }

    SECTION( "a generic field" )
    {
        const Parsed p( "struct Holder { Vector<i32> items; };" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
    }

    // Duplicate names and shadowed type spellings are sema's business, not the parser's: `i32` is
    // an ordinary identifier (D1), and a repeated field is the same kind of error as a repeated
    // parameter.
    SECTION( "the parser does not judge field names" )
    {
        REQUIRE_FALSE( Parsed( "struct P { f64 x; f64 x; };" ).has_errors() );
        REQUIRE_FALSE( Parsed( "struct P { i32 i32; };" ).has_errors() );
    }

    SECTION( "the trailing semicolon is required, as in C++" )
    {
        const Parsed p( "struct P { f64 x; }\ni32 main() { return 0; }\n" );

        INFO( p.errors() );
        REQUIRE( p.has_errors() );
    }

    // Declaration order carries no meaning between top-level declarations (D18), so a struct may
    // be used by a function above it.
    SECTION( "a struct declared after the function that uses it" )
    {
        const Parsed p( "i32 main() { return 0; }\nstruct Point { f64 x; };\n" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
    }
}

// Recovery, not just the message: synchronise() alone stops at the first `;` inside the struct,
// and the rest of the function is then read as top-level declarations.
TEST_CASE( "parser_reports_a_struct_inside_a_function_once", "[parse]" )
{
    const Parsed p( "i32 main()\n{\n    struct Local { i32 x; };\n    return 0;\n}\n" );

    INFO( p.errors() );
    REQUIRE( p.error_count() == 1 );
    REQUIRE( p.errors().find( "cannot be declared inside a function" ) != std::string::npos );

    SECTION( "and the rest of the function still parses" )
    {
        const Node_id body = p.child( p.child( p.root(), 0 ), 2 );

        REQUIRE( p.kind( body ) == Node_kind::Block );

        bool found_return = false;
        for( const Node_id stmt : p.children( body ) )
        {
            found_return = found_return || p.kind( stmt ) == Node_kind::Return_stmt;
        }

        REQUIRE( found_return );
    }
}

// A malformed one reports its own errors from inside parse_struct_decl, but must still be
// consumed whole rather than leaving the cursor mid-struct.
TEST_CASE( "parser_recovers_from_a_malformed_struct_inside_a_function", "[parse]" )
{
    const Parsed p( "i32 main()\n{\n    struct Local { i32 };\n    return 0;\n}\n" );

    INFO( p.errors() );
    REQUIRE( p.has_errors() );

    const Node_id body = p.child( p.child( p.root(), 0 ), 2 );

    bool found_return = false;
    for( const Node_id stmt : p.children( body ) )
    {
        found_return = found_return || p.kind( stmt ) == Node_kind::Return_stmt;
    }

    REQUIRE( found_return );
}

TEST_CASE( "parser_reports_malformed_structs", "[parse]" )
{
    struct Case
    {
        std::string_view source;
        std::string_view expected;
    };

    static const Case cases[] = {
        { "struct { f64 x; };", "expected" },      // no name
        { "struct Point f64 x; };", "expected" },  // no opening brace
        { "struct Point { f64 x; ", "expected" },  // unterminated
        { "struct Point { f64 x };", "expected" }, // field without a semicolon
        { "struct Point { f64; };", "expected" },  // field without a name
    };

    for( const Case& c : cases )
    {
        const Parsed p( c.source );

        INFO( "source: " << c.source << "\n" << p.errors() );
        REQUIRE( p.has_errors() );
        REQUIRE( p.errors().find( c.expected ) != std::string::npos );
    }
}

// A malformed struct must not swallow what follows it: recovery is what keeps one mistake from
// hiding every later one.
TEST_CASE( "parser_recovers_after_a_malformed_struct", "[parse]" )
{
    const Parsed p( "struct Broken { f64 };\ni32 main() { return 0; }\n" );

    INFO( p.errors() );
    REQUIRE( p.has_errors() );

    bool found_main = false;
    for( const Node_id decl : p.children( p.root() ) )
    {
        found_main = found_main || p.kind( decl ) == Node_kind::Function_decl;
    }

    REQUIRE( found_main );
}

TEST_CASE( "parser_rejects_a_forward_declaration", "[parse]" )
{
    SECTION( "one error, naming the brace" )
    {
        const Parsed p( "i32 odd( i32 n );\ni32 odd( i32 n ) { return n; }\n" );

        INFO( p.errors() );
        REQUIRE( p.error_count() == 1 );
        REQUIRE( p.errors().find( "expected `{`" ) != std::string::npos );
        REQUIRE( p.errors().find( "no forward declarations" ) != std::string::npos );
    }

    SECTION( "the declarations after it still parse" )
    {
        const Parsed p( "i32 f( i32 n );\ni32 g() { return 1; }\ni32 h() { return 2; }\n" );

        INFO( p.errors() );
        REQUIRE( p.error_count() == 1 );

        // The prototype becomes an Error node, so it declares nothing - the two real functions are
        // the only Function_decls, and sema will not see a duplicate `f`.
        std::size_t functions = 0;
        for( const Node_id decl : p.children( p.root() ) )
        {
            functions += p.kind( decl ) == Node_kind::Function_decl ? 1 : 0;
        }

        REQUIRE( functions == 2 );
    }
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

TEST_CASE( "parser_variable_declarations", "[parse]" )
{
    SECTION( "with an initialiser" )
    {
        const Parsed p( "i32 main() { i32 y = 0; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id decl = first_statement( p );
        REQUIRE( p.kind( decl ) == Node_kind::Var_decl );
        REQUIRE( p.children( decl ).size() == 2 );
        REQUIRE( p.kind( p.child( decl, 0 ) ) == Node_kind::Named_type );
        REQUIRE( p.kind( p.child( decl, 1 ) ) == Node_kind::Int_literal );
        REQUIRE( Symbol_id { p.aux( decl ) }.is_valid() );
    }

    // Fixed arity: always two children, the initialiser slot invalid when absent.
    SECTION( "without an initialiser" )
    {
        const Parsed p( "i32 main() { i32 y; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id decl = first_statement( p );
        REQUIRE( p.children( decl ).size() == 2 );
        REQUIRE( p.child( decl, 0 ).is_valid() );
        REQUIRE_FALSE( p.child( decl, 1 ).is_valid() );
    }

    // An inferred type is an invalid Node_id, not an Error node - `auto x = 1;` is not a mistake.
    SECTION( "auto leaves the type slot empty rather than an Error" )
    {
        const Parsed p( "i32 main() { auto x = 1; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id decl = first_statement( p );
        REQUIRE( p.kind( decl ) == Node_kind::Var_decl );
        REQUIRE_FALSE( p.child( decl, 0 ).is_valid() );
        REQUIRE( p.kind( p.child( decl, 1 ) ) == Node_kind::Int_literal );
    }

    // previous() is the `auto` keyword when expect() fails, so an unguarded read would silently
    // give the variable that symbol as its name. Only `auto` reaches this path: `i32 = 5;` is a
    // well-formed assignment to a variable called `i32`, since type names are ordinary identifiers.
    SECTION( "a missing name reports rather than reusing the previous token" )
    {
        const Parsed p( "i32 main() { auto = 5; }" );

        REQUIRE( p.has_errors() );
        INFO( p.errors() );
        REQUIRE( p.errors().find( "expected an identifier" ) != std::string::npos );
    }
}

TEST_CASE( "parser_assignment_and_increment", "[parse]" )
{
    SECTION( "plain assignment" )
    {
        const Parsed p( "i32 main() { y = 1; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id stmt = first_statement( p );
        REQUIRE( p.kind( stmt ) == Node_kind::Assign_stmt );
        REQUIRE( p.children( stmt ).size() == 2 );
        REQUIRE( static_cast<Token_kind>( p.aux( stmt ) ) == Token_kind::Equal );
    }

    // One node kind covers all eleven forms, so aux is the only thing distinguishing them.
    SECTION( "every compound form is recorded in aux" )
    {
        static const Token_kind operators[] = {
            Token_kind::Equal,
            Token_kind::Plus_equal,
            Token_kind::Minus_equal,
            Token_kind::Star_equal,
            Token_kind::Slash_equal,
            Token_kind::Percent_equal,
            Token_kind::Amp_equal,
            Token_kind::Pipe_equal,
            Token_kind::Caret_equal,
            Token_kind::Less_less_equal,
            Token_kind::Greater_greater_equal,
        };

        for( const Token_kind op : operators )
        {
            const std::string source = std::string( "i32 main() { y " ) + std::string( token_kind_spelling( op ) ) + " 1; }";
            const Parsed      p( source );

            INFO( source << "\n" << p.errors() );
            REQUIRE_FALSE( p.has_errors() );

            const Node_id stmt = first_statement( p );
            REQUIRE( p.kind( stmt ) == Node_kind::Assign_stmt );
            REQUIRE( static_cast<Token_kind>( p.aux( stmt ) ) == op );
        }
    }

    SECTION( "increment and decrement" )
    {
        for( const Token_kind op : { Token_kind::Plus_plus, Token_kind::Minus_minus } )
        {
            const Parsed p( std::string( "i32 main() { y " ) + std::string( token_kind_spelling( op ) ) + "; }" );

            INFO( p.errors() );
            REQUIRE_FALSE( p.has_errors() );

            const Node_id stmt = first_statement( p );
            REQUIRE( p.kind( stmt ) == Node_kind::Increment_stmt );
            REQUIRE( p.children( stmt ).size() == 1 );
            REQUIRE( static_cast<Token_kind>( p.aux( stmt ) ) == op );
        }
    }

    // Assignment is a statement, not an operator (L11), which is what makes `if( x = 5 )`
    // unparseable rather than needing a check.
    SECTION( "assignment is not an expression" )
    {
        const Parsed p( "i32 main() { return y = 1; }" );
        REQUIRE( p.has_errors() );
    }
}

// PLAN §6.3 D15.
TEST_CASE( "parser_expression_statements_must_have_an_effect", "[parse]" )
{
    SECTION( "a call is allowed" )
    {
        const Parsed p( "i32 main() { f(); }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.kind( first_statement( p ) ) == Node_kind::Expr_stmt );
    }

    SECTION( "a discarded value is not" )
    {
        for( const char* source : { "a + b;", "y;", "1;", "a < b;" } )
        {
            const Parsed p( std::string( "i32 main() { " ) + source + " }" );

            INFO( "source: " << source << "\n" << p.errors() );
            REQUIRE( p.has_errors() );
            REQUIRE( p.errors().find( "has no effect" ) != std::string::npos );
        }
    }

    SECTION( "an already-failed expression is not reported twice" )
    {
        const Parsed p( "i32 main() { f( 1; }" );

        INFO( p.errors() );
        REQUIRE( p.error_count() == 1 );
    }
}

// PLAN §6.3 D17: `*` and `&` bind to the type, so they must touch it.
TEST_CASE( "parser_pointer_and_reference_types", "[parse]" )
{
    SECTION( "adjacent forms parse" )
    {
        const Parsed p( "i32 main() { u32* p; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id type = p.child( first_statement( p ), 0 );
        REQUIRE( p.kind( type ) == Node_kind::Pointer_type );
        REQUIRE( p.kind( p.child( type, 0 ) ) == Node_kind::Named_type );
    }

    SECTION( "they nest" )
    {
        const Parsed p( "i32 main() { u32** p; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id outer = p.child( first_statement( p ), 0 );
        REQUIRE( p.kind( outer ) == Node_kind::Pointer_type );
        REQUIRE( p.kind( p.child( outer, 0 ) ) == Node_kind::Pointer_type );
    }

    // D32: `&` is address-of and nothing else, so `T&` is a hard error naming the replacement -
    // and exactly one error, not two, because the adjacency rule no longer applies to it.
    SECTION( "`T&` names `ref T` as the replacement" )
    {
        const Parsed p( "i32 main() { u32& r; }" );

        INFO( p.errors() );
        REQUIRE( p.has_errors() );
        REQUIRE( p.error_count() == 1 );
        REQUIRE( p.errors().find( "`&` is not a type" ) != std::string::npos );
        REQUIRE( p.errors().find( "write `ref T`" ) != std::string::npos );
    }

    // The spaced form is the one that would have collected a second, useless diagnostic.
    SECTION( "and the spaced form says the same thing, once" )
    {
        const Parsed p( "i32 main() { u32 & r; }" );

        INFO( p.errors() );
        REQUIRE( p.error_count() == 1 );
        REQUIRE( p.errors().find( "`&` is not a type" ) != std::string::npos );
    }

    // The message a C++ programmer's habit produces has to name the fix, not just say "no effect".
    SECTION( "the spaced form names the fix" )
    {
        for( const char* source : { "u32 *p;", "u32 * p;" } )
        {
            const Parsed p( std::string( "i32 main() { " ) + source + " }" );

            INFO( "source: " << source << "\n" << p.errors() );
            REQUIRE( p.has_errors() );
            REQUIRE( p.errors().find( "must touch the type it modifies" ) != std::string::npos );
            REQUIRE( p.errors().find( "write `u32* p`" ) != std::string::npos );
        }
    }

    SECTION( "a spaced `*` in a parameter or return type is rejected too" )
    {
        const Parsed p( "i32 f( u32 *p ) { return 0; }" );

        REQUIRE( p.has_errors() );
        INFO( p.errors() );
        REQUIRE( p.errors().find( "must touch the type it modifies" ) != std::string::npos );
    }
}

// The scan decides from shape alone - it never asks whether the leading name is a type (L17).
TEST_CASE( "parser_declaration_versus_expression", "[parse]" )
{
    struct Case
    {
        const char* source;
        Node_kind   kind;
    };

    static const Case cases[] = {
        { "i32 y = 0;", Node_kind::Var_decl },
        { "Point p;", Node_kind::Var_decl },
        { "Point* p;", Node_kind::Var_decl },
        { "const i32 x = 0;", Node_kind::Var_decl },
        { "Vector<i32> v;", Node_kind::Var_decl },
        { "Vector<Vector<i32>> v;", Node_kind::Var_decl },
        { "auto x = 1;", Node_kind::Var_decl },
        { "y = 1;", Node_kind::Assign_stmt },
        { "y++;", Node_kind::Increment_stmt },
        { "f();", Node_kind::Expr_stmt },
    };

    for( const Case& c : cases )
    {
        const Parsed p( std::string( "i32 main() { " ) + c.source + " }" );

        INFO( "source: " << c.source << "\n" << p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.kind( first_statement( p ) ) == c.kind );
    }
}

// Nesting works only because match_generic_close splits the `>>` the lexer produces - see
// lexer_generic_close_is_two_greaters, which pinned that behaviour before there was a consumer.
TEST_CASE( "parser_generic_and_qualified_types", "[parse]" )
{
    SECTION( "a single argument" )
    {
        const Parsed p( "i32 main() { Vector<i32> v; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id type = p.child( first_statement( p ), 0 );
        REQUIRE( p.kind( type ) == Node_kind::Generic_type );
        REQUIRE( p.children( type ).size() == 2 );
        REQUIRE( p.kind( p.child( type, 1 ) ) == Node_kind::Type_arg_list );
        REQUIRE( p.children( p.child( type, 1 ) ).size() == 1 );
    }

    SECTION( "several arguments" )
    {
        const Parsed p( "i32 main() { Map<i32, Point> m; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id args = p.child( p.child( first_statement( p ), 0 ), 1 );
        REQUIRE( p.children( args ).size() == 2 );
    }

    SECTION( "nested, closing with a single >> token" )
    {
        const Parsed p( "i32 main() { Vector<Vector<i32>> v; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id outer = p.child( first_statement( p ), 0 );
        const Node_id inner = p.child( p.child( outer, 1 ), 0 );
        REQUIRE( p.kind( inner ) == Node_kind::Generic_type );
    }

    SECTION( "const wraps the type it applies to" )
    {
        const Parsed p( "i32 main() { const i32* const q; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id type = p.child( first_statement( p ), 0 );
        REQUIRE( p.kind( type ) == Node_kind::Const_type );                             // trailing const
        REQUIRE( p.kind( p.child( type, 0 ) ) == Node_kind::Pointer_type );             // the `*`
        REQUIRE( p.kind( p.child( p.child( type, 0 ), 0 ) ) == Node_kind::Const_type ); // leading

        // The Named_type covers the identifier only, not the const in front of it.
        const Node_id named = find_first( p.ast(), first_statement( p ), Node_kind::Named_type );
        REQUIRE( p.text( named ) == "i32" );
    }

    SECTION( "an unclosed argument list reports" )
    {
        const Parsed p( "i32 main() { Vector<i32 v; }" );
        REQUIRE( p.has_errors() );
    }
}

// can_start_expression keeps the better message for tokens that cannot begin a statement.
TEST_CASE( "parser_reports_tokens_that_cannot_start_a_statement", "[parse]" )
{
    // `struct` is deliberately absent: it has its own message now, since a struct in a function
    // body is a specific mistake rather than an unrecognisable token.
    for( const char* source : { ";", ",", ")", "]" } )
    {
        const Parsed p( std::string( "i32 main() { " ) + source + " }" );

        INFO( "source: " << source << "\n" << p.errors() );
        REQUIRE( p.has_errors() );
        REQUIRE( p.errors().find( "expected a statement" ) != std::string::npos );
    }
}

TEST_CASE( "parser_unary_operators", "[parse]" )
{
    SECTION( "each operator is recorded in aux" )
    {
        static const Token_kind operators[] = {
            Token_kind::Minus,
            Token_kind::Bang,
            Token_kind::Tilde,
            Token_kind::Star,
            Token_kind::Amp,
        };

        for( const Token_kind op : operators )
        {
            const Parsed p( std::string( "i32 main() { return " ) + std::string( token_kind_spelling( op ) ) + "x; }" );

            INFO( token_kind_spelling( op ) << "\n" << p.errors() );
            REQUIRE_FALSE( p.has_errors() );

            const Node_id expression = parse_expression_of( p );
            REQUIRE( p.kind( expression ) == Node_kind::Unary_expr );
            REQUIRE( p.children( expression ).size() == 1 );
            REQUIRE( static_cast<Token_kind>( p.aux( expression ) ) == op );
        }
    }

    // The span used to cover only the operator, because previous() was read in the same argument
    // list that parsed the operand - and C++ leaves that evaluation order unspecified.
    SECTION( "the span covers the operand" )
    {
        const Parsed p( "i32 main() { return -abc; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id expression = parse_expression_of( p );
        REQUIRE( p.text( expression ) == "-abc" );

        // A parent must contain its child; the old span did not.
        REQUIRE( p.ast().span( expression ).contains( p.ast().span( p.child( expression, 0 ) ) ) );
    }

    // Unary binds tighter than every binary operator but looser than a call.
    SECTION( "precedence" )
    {
        REQUIRE( shape_of( "-a * b" ) == "*(-(a),b)" );
        REQUIRE( shape_of( "a * -b" ) == "*(a,-(b))" );
        REQUIRE( shape_of( "-a + b" ) == "+(-(a),b)" );
        REQUIRE( shape_of( "-f( 1 )" ) == "-(call(f,[1]))" );
        REQUIRE( shape_of( "!a == b" ) == "==(!(a),b)" );
    }

    SECTION( "right associative" )
    {
        REQUIRE( shape_of( "- -x" ) == "-(-(x))" );
        REQUIRE( shape_of( "!!x" ) == "!(!(x))" );
        REQUIRE( shape_of( "-!~x" ) == "-(!(~(x)))" );
    }

    // Passing the unary operator as `enclosing` would make this a spurious D16 error, since `-`
    // classifies as Additive and `<<` as Shift.
    SECTION( "does not trip the D16 mixing check" )
    {
        const Parsed p( "i32 main() { return -a << b; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
    }

    SECTION( "dereference works as an assignment target" )
    {
        const Parsed p( "i32 main() { *p = 1; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.kind( first_statement( p ) ) == Node_kind::Assign_stmt );
    }
}

// --- written before the implementation: these pin the intended shapes ---

TEST_CASE( "parser_if_statement", "[parse]" )
{
    SECTION( "without an else, the third child is invalid" )
    {
        const Parsed p( "i32 main() { if( x ) { return 1; } }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id stmt = first_statement( p );
        REQUIRE( p.kind( stmt ) == Node_kind::If_stmt );
        REQUIRE( p.children( stmt ).size() == 3 );
        REQUIRE( p.kind( p.child( stmt, 1 ) ) == Node_kind::Block );
        REQUIRE_FALSE( p.child( stmt, 2 ).is_valid() );
    }

    SECTION( "with an else" )
    {
        const Parsed p( "i32 main() { if( x ) { return 1; } else { return 2; } }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id stmt = first_statement( p );
        REQUIRE( p.kind( p.child( stmt, 2 ) ) == Node_kind::Block );
    }

    // The one exception to D3's mandatory braces: `else if` chains rather than nesting a block.
    SECTION( "else if chains" )
    {
        const Parsed p( "i32 main() { if( a ) { return 1; } else if( b ) { return 2; } else { return 3; } }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id outer = first_statement( p );
        const Node_id inner = p.child( outer, 2 );
        REQUIRE( p.kind( inner ) == Node_kind::If_stmt );
        REQUIRE( p.kind( p.child( inner, 2 ) ) == Node_kind::Block );
    }

    // PLAN §6.3 D3.
    SECTION( "a braceless body is rejected" )
    {
        for( const char* source : { "if( x ) return 1;", "if( x ) { } else return 2;" } )
        {
            const Parsed p( std::string( "i32 main() { " ) + source + " }" );

            INFO( "source: " << source << "\n" << p.errors() );
            REQUIRE( p.has_errors() );
        }
    }

    SECTION( "a missing condition reports" )
    {
        for( const char* source : { "if x { }", "if( ) { }", "if( x { }" } )
        {
            const Parsed p( std::string( "i32 main() { " ) + source + " }" );
            INFO( "source: " << source << "\n" << p.errors() );
            REQUIRE( p.has_errors() );
        }
    }
}

TEST_CASE( "parser_while_statement", "[parse]" )
{
    SECTION( "condition and body" )
    {
        const Parsed p( "i32 main() { while( y < x ) { y++; } }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id stmt = first_statement( p );
        REQUIRE( p.kind( stmt ) == Node_kind::While_stmt );
        REQUIRE( p.children( stmt ).size() == 2 );
        REQUIRE( p.kind( p.child( stmt, 0 ) ) == Node_kind::Binary_expr );
        REQUIRE( p.kind( p.child( stmt, 1 ) ) == Node_kind::Block );
    }

    SECTION( "a braceless body is rejected" )
    {
        const Parsed p( "i32 main() { while( x ) y++; }" );
        REQUIRE( p.has_errors() );
    }
}

TEST_CASE( "parser_for_statement", "[parse]" )
{
    SECTION( "all four clauses" )
    {
        const Parsed p( "i32 main() { for( i32 i = 0; i < 3; i++ ) { y = y + i; } }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id stmt = first_statement( p );
        REQUIRE( p.kind( stmt ) == Node_kind::For_stmt );
        REQUIRE( p.children( stmt ).size() == 4 );
        REQUIRE( p.kind( p.child( stmt, 0 ) ) == Node_kind::Var_decl );
        REQUIRE( p.kind( p.child( stmt, 1 ) ) == Node_kind::Binary_expr );
        REQUIRE( p.kind( p.child( stmt, 2 ) ) == Node_kind::Increment_stmt );
        REQUIRE( p.kind( p.child( stmt, 3 ) ) == Node_kind::Block );
    }

    // Empty clauses keep their slots, so the arity stays fixed.
    SECTION( "empty clauses" )
    {
        const Parsed p( "i32 main() { for( ; ; ) { } }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id stmt = first_statement( p );
        REQUIRE( p.children( stmt ).size() == 4 );
        REQUIRE_FALSE( p.child( stmt, 0 ).is_valid() );
        REQUIRE_FALSE( p.child( stmt, 1 ).is_valid() );
        REQUIRE_FALSE( p.child( stmt, 2 ).is_valid() );
        REQUIRE( p.child( stmt, 3 ).is_valid() );
    }

    SECTION( "an assignment as the init clause" )
    {
        const Parsed p( "i32 main() { for( i = 0; i < 3; i++ ) { } }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.kind( p.child( first_statement( p ), 0 ) ) == Node_kind::Assign_stmt );
    }

    // The update clause is followed by `)`, not `;` - the one place the statement rules cannot be
    // reused unchanged.
    SECTION( "the update clause takes no semicolon" )
    {
        const Parsed p( "i32 main() { for( ; ; i++; ) { } }" );
        REQUIRE( p.has_errors() );
    }

    SECTION( "a braceless body is rejected" )
    {
        const Parsed p( "i32 main() { for( ; ; ) y++; }" );
        REQUIRE( p.has_errors() );
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

// `break` and `continue` are statements, not expressions, so they are matched before the
// expression fallback - which is why can_start_expression needs nothing for them. The first
// feature that has not had to edit both halves of that list.
TEST_CASE( "parser_parses_break_and_continue", "[parse][loops]" )
{
    SECTION( "both, with no children and no aux" )
    {
        const Parsed  p( "i32 main() { while ( true ) { break; continue; } return 0; }" );
        const Node_id brk  = find_first( p.ast(), p.root(), Node_kind::Break_stmt );
        const Node_id cont = find_first( p.ast(), p.root(), Node_kind::Continue_stmt );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( brk.is_valid() );
        REQUIRE( cont.is_valid() );
        REQUIRE( p.children( brk ).empty() );
        REQUIRE( p.children( cont ).empty() );
    }

    // The span carries the semicolon, so a diagnostic underlines the whole statement.
    SECTION( "the span covers the whole statement" )
    {
        const Parsed  p( "i32 main() { while ( true ) { break; } return 0; }" );
        const Node_id brk = find_first( p.ast(), p.root(), Node_kind::Break_stmt );

        INFO( p.dump() );
        REQUIRE( p.text( brk ) == "break;" );
    }

    SECTION( "in a for body" )
    {
        const Parsed p( "i32 main() { for ( i32 i = 0; i < 3; i++ ) { continue; } return 0; }" );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( find_first( p.ast(), p.root(), Node_kind::Continue_stmt ).is_valid() );
    }

    SECTION( "a missing semicolon is reported" )
    {
        for( const std::string_view source :
             { "i32 main() { while ( true ) { break } return 0; }", "i32 main() { while ( true ) { continue } return 0; }" } )
        {
            INFO( "source '" << source << "'" );
            const Parsed p( source );

            REQUIRE( p.root().is_valid() ); // reached only if parsing terminated
            REQUIRE( p.has_errors() );
        }
    }

    // Neither takes an operand, so the keyword must not swallow what follows it.
    SECTION( "neither takes a value" )
    {
        const Parsed p( "i32 main() { while ( true ) { break 1; } return 0; }" );

        INFO( p.dump() );
        REQUIRE( p.has_errors() );
    }
}

// At file scope a function and a variable both open with a type and a name; only what follows
// the name tells them apart. Getting that wrong in either direction is silent - a variable read as
// a function, or a function read as a variable - so both sides are pinned.
TEST_CASE( "parser_parses_file_scope_variables", "[parse][globals]" )
{
    SECTION( "a variable with an initialiser" )
    {
        const Parsed  p( "i32 counter = 1;" );
        const Node_id decl = find_first( p.ast(), p.root(), Node_kind::Var_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( decl.is_valid() );
        REQUIRE( p.kind( p.child( decl, 0 ) ) == Node_kind::Named_type );
        REQUIRE( p.kind( p.child( decl, 1 ) ) == Node_kind::Int_literal );
    }

    SECTION( "and one without" )
    {
        const Parsed p( "i32 counter;" );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( find_first( p.ast(), p.root(), Node_kind::Var_decl ).is_valid() );
    }

    // The other side of the same decision: `(` after the name still means a function.
    SECTION( "a function is still a function" )
    {
        const Parsed p( "i32 main() { return 0; }" );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( find_first( p.ast(), p.root(), Node_kind::Function_decl ).is_valid() );
        REQUIRE_FALSE( find_first( p.ast(), p.root(), Node_kind::Var_decl ).is_valid() );
    }

    SECTION( "a pointer type" )
    {
        const Parsed  p( "i32* address = nullptr;" );
        const Node_id decl = find_first( p.ast(), p.root(), Node_kind::Var_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.kind( p.child( decl, 0 ) ) == Node_kind::Pointer_type );
    }

    // const still parses at file scope, even though nothing enforces it yet.
    SECTION( "a const type" )
    {
        const Parsed  p( "const i32 fixed = 5;" );
        const Node_id decl = find_first( p.ast(), p.root(), Node_kind::Var_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( decl.is_valid() );
    }

    SECTION( "several, mixed with functions and structs" )
    {
        const Parsed p( "i32 first = 1;\n"
                        "struct Point { i32 x; };\n"
                        "i32 second = 2;\n"
                        "i32 main() { return first; }\n"
                        "i32 third = 3;\n" );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( p.root() ).size() == 5 );
    }

    SECTION( "a missing semicolon is reported" )
    {
        const Parsed p( "i32 counter = 1" );

        REQUIRE( p.root().is_valid() ); // reached only if parsing terminated
        REQUIRE( p.has_errors() );
    }

    // The scan must put the cursor back when it does not find a declaration.
    SECTION( "junk at file scope still reports once" )
    {
        for( const std::string_view source : { "= 1;", "i32 = 1;", "i32;", "1 + 1;" } )
        {
            INFO( "source '" << source << "'" );
            const Parsed p( source );

            REQUIRE( p.root().is_valid() );
            REQUIRE( p.has_errors() );
        }
    }
}

TEST_CASE( "parser_reports_each_mistake_once", "[parse]" )
{
    const Parsed p( "i32 main() { return 0 }" );

    INFO( p.errors() );
    REQUIRE( p.error_count() == 1 );
}

// `cast` and `wrap` are keywords precisely so the `<` that follows can only be a bracket. As
// identifiers they would inherit C++'s `a < b > ( c )` ambiguity, which is the reason for the
// choice - so the shape of what they parse into is worth pinning.
TEST_CASE( "parser_parses_cast_and_wrap", "[parse]" )
{
    SECTION( "children are the type then the operand" )
    {
        const Parsed  p( "i32 main() { return cast<i64>( x ); }" );
        const Node_id conversion = find_first( p.ast(), p.root(), Node_kind::Cast_expr );

        INFO( p.dump() );
        REQUIRE( !p.has_errors() );
        REQUIRE( conversion.is_valid() );
        REQUIRE( p.children( conversion ).size() == 2 );
        REQUIRE( p.kind( p.child( conversion, 0 ) ) == Node_kind::Named_type );
        REQUIRE( p.kind( p.child( conversion, 1 ) ) == Node_kind::Name_expr );
    }

    // One node kind covers both, so aux is the only thing that tells them apart. With it left at
    // zero every `wrap` would check and lower as a `cast`.
    SECTION( "aux records which operator it was" )
    {
        const Parsed  c( "i32 main() { return cast<i64>( x ); }" );
        const Parsed  w( "i32 main() { return wrap<i64>( x ); }" );
        const Node_id from_cast = find_first( c.ast(), c.root(), Node_kind::Cast_expr );
        const Node_id from_wrap = find_first( w.ast(), w.root(), Node_kind::Cast_expr );

        REQUIRE( c.aux( from_cast ) == static_cast<u32>( Keyword::Cast ) );
        REQUIRE( w.aux( from_wrap ) == static_cast<u32>( Keyword::Wrap ) );
        REQUIRE( c.aux( from_cast ) != w.aux( from_wrap ) );
    }

    SECTION( "a pointer target keeps its star" )
    {
        const Parsed  p( "i32 main() { return cast<i32*>( x ); }" );
        const Node_id conversion = find_first( p.ast(), p.root(), Node_kind::Cast_expr );

        INFO( p.dump() );
        REQUIRE( !p.has_errors() );
        REQUIRE( p.kind( p.child( conversion, 0 ) ) == Node_kind::Pointer_type );
    }

    // match_generic_close, not expect( Greater ): the lexer hands over one `>>` token here.
    SECTION( "a nested target closes on >>" )
    {
        const Parsed  p( "i32 main() { return cast<Vector<i32>>( x ); }" );
        const Node_id conversion = find_first( p.ast(), p.root(), Node_kind::Cast_expr );

        INFO( p.dump() );
        REQUIRE( !p.has_errors() );
        REQUIRE( p.kind( p.child( conversion, 0 ) ) == Node_kind::Generic_type );
    }

    // The parens belong to the operator, so this is not a call and `.x` applies to the result.
    SECTION( "postfix still applies to the result" )
    {
        const Parsed p( "i32 main() { return cast<Point>( q ).x; }" );

        INFO( p.dump() );
        REQUIRE( !p.has_errors() );

        const Node_id field = find_first( p.ast(), p.root(), Node_kind::Field_expr );

        REQUIRE( field.is_valid() );
        REQUIRE( p.kind( p.child( field, 0 ) ) == Node_kind::Cast_expr );
    }

    // can_start_expression gates statement position, and it is the half that keeps being forgotten
    // when the other half gains a keyword. A conversion really has no effect, so the message that
    // survives is that one - not "expected a statement", which is what a missing keyword gives.
    SECTION( "it can open a statement" )
    {
        const Parsed p( "i32 main() { wrap<u8>( x ); return 0; }" );

        INFO( p.errors() );

        REQUIRE( find_first( p.ast(), p.root(), Node_kind::Cast_expr ).is_valid() );
        REQUIRE( p.errors().find( "no effect" ) != std::string::npos );
        REQUIRE( p.errors().find( "expected a statement" ) == std::string::npos );
    }

    SECTION( "a missing type list is an error, not a hang" )
    {
        for( const std::string_view source :
             { "i32 main() { return cast( x ); }",
               "i32 main() { return cast<i32( x ); }",
               "i32 main() { return wrap<>( x ); }",
               "i32 main() { return cast<i32> x; }" } )
        {
            INFO( "source '" << source << "'" );
            const Parsed p( source );

            REQUIRE( p.root().is_valid() ); // reached only if parsing terminated
            REQUIRE( p.has_errors() );
        }
    }
}

// D29 gives a struct and a class different rules and the same shape, so the parser's job is only
// to record which keyword was written and to accept a destructor as a member. Every rule about
// which may have one belongs to the checker, which can say why.
TEST_CASE( "parser_parses_class_declarations", "[parse][aggregates]" )
{
    SECTION( "a class is its own node kind, holding the same fields a struct would" )
    {
        const Parsed  p( "class Buffer { u8* ptr; u64 len; };" );
        const Node_id decl = find_first( p.ast(), p.root(), Node_kind::Class_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( decl.is_valid() );
        REQUIRE( p.children( decl ).size() == 2 );
        REQUIRE( p.kind( p.child( decl, 0 ) ) == Node_kind::Field_decl );
        REQUIRE( p.kind( p.child( decl, 1 ) ) == Node_kind::Field_decl );
    }

    SECTION( "a struct is unchanged" )
    {
        const Parsed p( "struct Point { i32 x; i32 y; };" );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( find_first( p.ast(), p.root(), Node_kind::Struct_decl ).is_valid() );
        REQUIRE_FALSE( find_first( p.ast(), p.root(), Node_kind::Class_decl ).is_valid() );
    }

    // The predicate is what keeps the passes that treat the two identically - field layout,
    // containment ordering, C emission - at one edit site rather than ten.
    SECTION( "is_aggregate covers both and nothing else" )
    {
        REQUIRE( is_aggregate( Node_kind::Struct_decl ) );
        REQUIRE( is_aggregate( Node_kind::Class_decl ) );
        REQUIRE_FALSE( is_aggregate( Node_kind::Field_decl ) );
        REQUIRE_FALSE( is_aggregate( Node_kind::Function_decl ) );
    }

    SECTION( "the name goes in aux, as a struct's does" )
    {
        const Parsed p( "class Buffer { u8* ptr; };\nstruct Point { i32 x; };" );

        const Node_id cls = find_first( p.ast(), p.root(), Node_kind::Class_decl );
        const Node_id str = find_first( p.ast(), p.root(), Node_kind::Struct_decl );

        INFO( p.dump() );
        REQUIRE( Symbol_id { p.aux( cls ) }.is_valid() );
        REQUIRE( Symbol_id { p.aux( str ) }.is_valid() );
        REQUIRE( p.aux( cls ) != p.aux( str ) );
    }

    // `class x;` is C++'s forward declaration and Keel has no answer for it yet. The struct path
    // already swallows the terminator so it does not come back as a stray `;`; the class path
    // must not diverge.
    SECTION( "a body-less class is an error, and eats its semicolon" )
    {
        const Parsed p( "class Buffer;" );

        INFO( p.dump() );
        REQUIRE( p.has_errors() );
        REQUIRE( p.error_count() == 1 );
    }

    SECTION( "an empty class body is legal" )
    {
        const Parsed  p( "class Empty { };" );
        const Node_id decl = find_first( p.ast(), p.root(), Node_kind::Class_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( decl ).empty() );
    }
}

// A destructor is shaped exactly like a Function_decl minus its return type, so that lower()'s
// linear scan finds one nested in a class body and reads its parameters and body from the same
// indices. The always-invalid first child is what buys that.
TEST_CASE( "parser_parses_destructors", "[parse][aggregates]" )
{
    SECTION( "shaped like a function, with no return type" )
    {
        const Parsed  p( "class Buffer { u8* ptr; ~Buffer() { } };" );
        const Node_id dtor = find_first( p.ast(), p.root(), Node_kind::Destructor_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( dtor.is_valid() );
        REQUIRE( p.children( dtor ).size() == 3 );
        REQUIRE_FALSE( p.child( dtor, 0 ).is_valid() ); // no return type
        REQUIRE( p.kind( p.child( dtor, 1 ) ) == Node_kind::Param_list );

        // C++ writes `this` implicitly; Keel writes it down, so every later pass sees an ordinary
        // parameter rather than a concept it has to know about.
        REQUIRE( p.children( p.child( dtor, 1 ) ).size() == 1 );

        const Node_id receiver = p.child( p.child( dtor, 1 ), 0 );

        REQUIRE( p.kind( receiver ) == Node_kind::Param_decl );
        REQUIRE( p.kind( p.child( receiver, 0 ) ) == Node_kind::Pointer_type );
        REQUIRE( p.kind( p.child( dtor, 2 ) ) == Node_kind::Block );
    }

    SECTION( "is_function_like covers it alongside a function" )
    {
        REQUIRE( is_function_like( Node_kind::Function_decl ) );
        REQUIRE( is_function_like( Node_kind::Destructor_decl ) );
        REQUIRE_FALSE( is_function_like( Node_kind::Class_decl ) );
        REQUIRE_FALSE( is_function_like( Node_kind::Field_decl ) );
    }

    // aux carries the name written after the `~`, so the checker can reject `~Wrong()` by
    // comparing it against the enclosing declaration rather than failing to parse.
    SECTION( "aux is the name after the tilde" )
    {
        const Parsed p( "class Buffer { ~Buffer() { } };" );

        const Node_id cls  = find_first( p.ast(), p.root(), Node_kind::Class_decl );
        const Node_id dtor = find_first( p.ast(), p.root(), Node_kind::Destructor_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.aux( dtor ) == p.aux( cls ) );
    }

    SECTION( "a mismatched name still parses - the checker is what rejects it" )
    {
        const Parsed p( "class Buffer { ~Wrong() { } };" );

        const Node_id cls  = find_first( p.ast(), p.root(), Node_kind::Class_decl );
        const Node_id dtor = find_first( p.ast(), p.root(), Node_kind::Destructor_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( dtor.is_valid() );
        REQUIRE( p.aux( dtor ) != p.aux( cls ) );
    }

    // The parser has no business knowing D29's rules, and a parse error here would give a worse
    // message than the checker's, which can name `class` as the fix.
    SECTION( "a destructor on a struct parses" )
    {
        const Parsed p( "struct Point { i32 x; ~Point() { } };" );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( find_first( p.ast(), p.root(), Node_kind::Destructor_decl ).is_valid() );
    }

    SECTION( "the body is a real block" )
    {
        const Parsed  p( "void release( u8* p ) { }\nclass Buffer { u8* ptr; ~Buffer() { release( ptr ); } };" );
        const Node_id dtor = find_first( p.ast(), p.root(), Node_kind::Destructor_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( find_first( p.ast(), p.child( dtor, 2 ), Node_kind::Call_expr ).is_valid() );
    }

    SECTION( "fields may follow a destructor as well as precede it" )
    {
        const Parsed  p( "class Buffer { ~Buffer() { } u8* ptr; };" );
        const Node_id decl = find_first( p.ast(), p.root(), Node_kind::Class_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( decl ).size() == 2 );
        REQUIRE( p.kind( p.child( decl, 0 ) ) == Node_kind::Destructor_decl );
        REQUIRE( p.kind( p.child( decl, 1 ) ) == Node_kind::Field_decl );
    }

    // C++ requires the parens and §5.1 keeps them: an empty parameter list that differed from
    // C++'s would be a divergence with nothing behind it.
    SECTION( "the parentheses are required" )
    {
        const Parsed p( "class Buffer { ~Buffer { } };" );

        INFO( p.dump() );
        REQUIRE( p.has_errors() );
    }

    SECTION( "a destructor takes no parameters of its own" )
    {
        const Parsed p( "class Buffer { ~Buffer( i32 n ) { } };" );

        INFO( p.dump() );
        REQUIRE( p.has_errors() );
    }

    // The receiver is the enclosing type's, not the name written after the `~`. Getting this wrong
    // would put a second, confusing error behind the mismatch diagnostic.
    SECTION( "a mismatched destructor still receives the enclosing type" )
    {
        const Parsed  p( "class Buffer { ~Wrong() { } };" );
        const Node_id dtor     = find_first( p.ast(), p.root(), Node_kind::Destructor_decl );
        const Node_id receiver = p.child( p.child( dtor, 1 ), 0 );
        const Node_id named    = p.child( p.child( receiver, 0 ), 0 );

        INFO( p.dump() );
        REQUIRE( p.kind( named ) == Node_kind::Named_type );
        REQUIRE( p.aux( named ) == p.aux( find_first( p.ast(), p.root(), Node_kind::Class_decl ) ) );
    }

    // A keyword, so `i32 this = 1;` and a field named `this` are not expressible.
    SECTION( "`this` in the body resolves through the ordinary name path" )
    {
        const Parsed  p( "class Buffer { u8* ptr; ~Buffer() { this.ptr = nullptr; } };" );
        const Node_id field = find_first( p.ast(), p.root(), Node_kind::Field_expr );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( field.is_valid() );
        REQUIRE( p.kind( p.child( field, 0 ) ) == Node_kind::Name_expr );
    }

    SECTION( "a nameless destructor is an error rather than a nameless declaration" )
    {
        const Parsed p( "class Buffer { ~() { } };" );

        INFO( p.dump() );
        REQUIRE( p.has_errors() );
        REQUIRE( p.root().is_valid() ); // reached only if parsing terminated
    }

    // The member loop advances on no progress so that garbage cannot spin it. A malformed
    // destructor is the new way to produce no progress, so it has to be pinned.
    SECTION( "a malformed destructor terminates the member loop" )
    {
        for( const std::string_view source :
             { "class Buffer { ~ };", "class Buffer { ~Buffer( };", "class Buffer { ~Buffer() };", "class Buffer { ~~ };" } )
        {
            INFO( "source '" << source << "'" );
            const Parsed p( source );

            REQUIRE( p.root().is_valid() ); // reached only if parsing terminated
            REQUIRE( p.has_errors() );
        }
    }

    SECTION( "the span covers the whole destructor" )
    {
        const Parsed  p( "class Buffer { ~Buffer() { } };" );
        const Node_id dtor = find_first( p.ast(), p.root(), Node_kind::Destructor_decl );

        INFO( p.dump() );
        REQUIRE( p.text( dtor ) == "~Buffer() { }" );
    }
}

// A constructor is decided on shape, not on name: `Identifier (` cannot be a field, which is
// `Type name;`. Keying on the name instead would send `Wrong( u64 n ) {}` to parse_field_decl and
// produce nonsense rather than the mismatch diagnostic the checker gives it.
TEST_CASE( "parser_parses_constructors", "[parse][aggregates]" )
{
    SECTION( "shaped like a function, with no return type" )
    {
        const Parsed  p( "class Buffer { u64 len; Buffer( u64 n ) { len = n; } };" );
        const Node_id ctor = find_first( p.ast(), p.root(), Node_kind::Constructor_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( ctor.is_valid() );
        REQUIRE( p.children( ctor ).size() == 3 );
        REQUIRE_FALSE( p.child( ctor, 0 ).is_valid() ); // no return type
        REQUIRE( p.kind( p.child( ctor, 1 ) ) == Node_kind::Param_list );
        REQUIRE( p.kind( p.child( ctor, 2 ) ) == Node_kind::Block );
    }

    SECTION( "is_function_like covers it" )
    {
        REQUIRE( is_function_like( Node_kind::Constructor_decl ) );
        REQUIRE_FALSE( is_aggregate( Node_kind::Constructor_decl ) );
    }

    // The receiver is first, then the author's own parameters - unlike a destructor, which may
    // have none of its own.
    SECTION( "the receiver comes first, then the declared parameters" )
    {
        const Parsed  p( "class Buffer { u64 len; Buffer( u64 n, i32 flag ) { } };" );
        const Node_id ctor     = find_first( p.ast(), p.root(), Node_kind::Constructor_decl );
        const Node_id params   = p.child( ctor, 1 );
        const Node_id receiver = p.child( params, 0 );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( params ).size() == 3 );
        REQUIRE( p.kind( p.child( receiver, 0 ) ) == Node_kind::Pointer_type );
    }

    SECTION( "no parameters of its own is fine" )
    {
        const Parsed  p( "class Buffer { u64 len; Buffer() { len = 0; } };" );
        const Node_id ctor = find_first( p.ast(), p.root(), Node_kind::Constructor_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( p.child( ctor, 1 ) ).size() == 1 ); // just the receiver
    }

    SECTION( "a field is still a field" )
    {
        const Parsed  p( "class Buffer { u64 len; u8* ptr; };" );
        const Node_id decl = find_first( p.ast(), p.root(), Node_kind::Class_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( decl ).size() == 2 );
        REQUIRE( p.kind( p.child( decl, 0 ) ) == Node_kind::Field_decl );
        REQUIRE( p.kind( p.child( decl, 1 ) ) == Node_kind::Field_decl );
    }

    // Decided on shape, so this parses and the checker reports it - the same treatment `~Wrong()`
    // gets, and for the same reason.
    SECTION( "a mismatched name still parses" )
    {
        const Parsed  p( "class Buffer { u64 len; Wrong( u64 n ) { } };" );
        const Node_id ctor = find_first( p.ast(), p.root(), Node_kind::Constructor_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( ctor.is_valid() );
        REQUIRE( p.aux( ctor ) != p.aux( find_first( p.ast(), p.root(), Node_kind::Class_decl ) ) );
    }

    SECTION( "the receiver is the enclosing type even when the name is wrong" )
    {
        const Parsed  p( "class Buffer { u64 len; Wrong( u64 n ) { } };" );
        const Node_id ctor     = find_first( p.ast(), p.root(), Node_kind::Constructor_decl );
        const Node_id receiver = p.child( p.child( ctor, 1 ), 0 );
        const Node_id named    = p.child( p.child( receiver, 0 ), 0 );

        INFO( p.dump() );
        REQUIRE( p.aux( named ) == p.aux( find_first( p.ast(), p.root(), Node_kind::Class_decl ) ) );
    }

    SECTION( "a constructor on a struct parses - the checker is what rejects it" )
    {
        const Parsed p( "struct Point { i32 x; Point( i32 a ) { x = a; } };" );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( find_first( p.ast(), p.root(), Node_kind::Constructor_decl ).is_valid() );
    }

    SECTION( "it may sit before the fields, and beside a destructor" )
    {
        const Parsed  p( "class Buffer { Buffer( u64 n ) { } ~Buffer() { } u64 len; };" );
        const Node_id decl = find_first( p.ast(), p.root(), Node_kind::Class_decl );

        INFO( p.dump() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( decl ).size() == 3 );
        REQUIRE( p.kind( p.child( decl, 0 ) ) == Node_kind::Constructor_decl );
        REQUIRE( p.kind( p.child( decl, 1 ) ) == Node_kind::Destructor_decl );
        REQUIRE( p.kind( p.child( decl, 2 ) ) == Node_kind::Field_decl );
    }

    // The member loop advances on no progress so that garbage cannot spin it, and a malformed
    // constructor is a new way to make none.
    SECTION( "a malformed constructor terminates the member loop" )
    {
        for( const std::string_view source :
             { "class Buffer { Buffer( };",
               "class Buffer { Buffer() };",
               "class Buffer { Buffer( u64 ) { } };",
               "class Buffer { Buffer( u64 n ) };" } )
        {
            INFO( "source '" << source << "'" );
            const Parsed p( source );

            REQUIRE( p.root().is_valid() ); // reached only if parsing terminated
            REQUIRE( p.has_errors() );
        }
    }
}

// PLAN D32. `ref` is a binding mode in every position, so it opens a local declaration as well as a
// parameter. It needs no lookahead the way a type name does: as an expression a bare marker has no
// effect and D15 rejects it, so `ref` at the start of a statement is always a declaration.
TEST_CASE( "parser_parses_a_ref_binding", "[parse]" )
{
    SECTION( "in statement position" )
    {
        const Parsed p( "void f( i32 x ) { ref i32 r = x; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id var = find_first( p.ast(), p.root(), Node_kind::Var_decl );

        REQUIRE( var.is_valid() );

        // The mode wraps the type, exactly as it does on a parameter - one node kind for both,
        // which is what keeps sema from having to ask where a binding was declared.
        const Node_id annotation = p.child( var, 0 );

        REQUIRE( p.kind( annotation ) == Node_kind::Mode_type );
        REQUIRE( p.aux( annotation ) == static_cast<u32>( Keyword::Ref ) );
        REQUIRE( p.kind( p.child( annotation, 0 ) ) == Node_kind::Named_type );
    }

    // The `for` init is the second place a declaration may start, and the two have needed editing
    // together before.
    SECTION( "in a for initialiser" )
    {
        const Parsed p( "void f( i32 x ) { for( ref i32 r = x; r < 10; r++ ) { } }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id var = find_first( p.ast(), p.root(), Node_kind::Var_decl );

        REQUIRE( var.is_valid() );
        REQUIRE( p.kind( p.child( var, 0 ) ) == Node_kind::Mode_type );
    }

    // `move` and `out` reach sema rather than failing here, so the message can say what a mode
    // means in this position instead of being a parse error about an unexpected keyword.
    SECTION( "and the other modes parse, to be rejected later" )
    {
        const Parsed p( "i32 main() { move i32 x = 1; return 0; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id var = find_first( p.ast(), p.root(), Node_kind::Var_decl );

        REQUIRE( var.is_valid() );
        REQUIRE( p.aux( p.child( var, 0 ) ) == static_cast<u32>( Keyword::Move ) );
    }
}

// PLAN D32. `const ref T` is the read-only borrow, and it is spelled in one order - `const ref i32`
// where C++ has `const i32&` and `i32 const&` meaning the same thing. The const wraps the mode, so
// the annotation is Const_type( Mode_type( T ) ) and every question about the mode has to see
// through one layer.
TEST_CASE( "parser_parses_a_const_ref", "[parse]" )
{
    SECTION( "as a parameter" )
    {
        const Parsed p( "i32 peek( const ref i32 n ) { return n; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id param = find_first( p.ast(), p.root(), Node_kind::Param_decl );

        REQUIRE( param.is_valid() );

        const Node_id annotation = p.child( param, 0 );

        REQUIRE( p.kind( annotation ) == Node_kind::Const_type );

        const Node_id mode = p.child( annotation, 0 );

        REQUIRE( p.kind( mode ) == Node_kind::Mode_type );
        REQUIRE( p.aux( mode ) == static_cast<u32>( Keyword::Ref ) );
        REQUIRE( p.kind( p.child( mode, 0 ) ) == Node_kind::Named_type );
    }

    SECTION( "as a local binding" )
    {
        const Parsed p( "void f( i32 x ) { const ref i32 r = x; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id var = find_first( p.ast(), p.root(), Node_kind::Var_decl );

        REQUIRE( var.is_valid() );
        REQUIRE( p.kind( p.child( var, 0 ) ) == Node_kind::Const_type );
        REQUIRE( p.kind( p.child( p.child( var, 0 ), 0 ) ) == Node_kind::Mode_type );
    }

    // One order, so the other is a hard error naming the replacement rather than a second spelling
    // of the same thing - the redundancy D25, D30 and D32 all refuse.
    SECTION( "and the other order is refused" )
    {
        const Parsed p( "i32 peek( ref const i32 n ) { return n; }" );

        INFO( p.errors() );
        REQUIRE( p.has_errors() );
        REQUIRE( p.errors().find( "const ref" ) != std::string::npos );
    }
}

// PLAN D32 and §8. A return type is a binding position like any other, so `const ref T` is spelled
// there the same way and builds the same Const_type( Mode_type( T ) ).
TEST_CASE( "parser_parses_a_const_ref_return", "[parse]" )
{
    SECTION( "the return type carries the mode" )
    {
        const Parsed p( "const ref i32 pick( const ref i32 a ) { return a; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id fn = find_first( p.ast(), p.root(), Node_kind::Function_decl );

        REQUIRE( fn.is_valid() );

        const Node_id annotation = p.child( fn, 0 );

        REQUIRE( p.kind( annotation ) == Node_kind::Const_type );
        REQUIRE( p.kind( p.child( annotation, 0 ) ) == Node_kind::Mode_type );
        REQUIRE( p.aux( p.child( annotation, 0 ) ) == static_cast<u32>( Keyword::Ref ) );
    }

    // A plain `ref` return parses, so sema can say only a `const ref` may be returned rather than
    // the parser producing a cascade about an unexpected keyword.
    SECTION( "and a plain ref return parses, to be rejected later" )
    {
        const Parsed p( "ref i32 pick( ref i32 a ) { return a; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
    }
}

// §8: a reference may not be stored in a struct. D32 makes that disappear into the grammar rather
// than needing a rule - a field is not a binding, so there is nowhere for a mode to go, and `ref`
// in field position is simply not a name.
TEST_CASE( "parser_refuses_a_reference_field", "[parse]" )
{
    const Parsed p( "struct S { ref i32 r; };\ni32 main() { return 0; }" );

    INFO( p.errors() );
    REQUIRE( p.has_errors() );
}

// PLAN D30. One `enum` keyword carrying `enum class` semantics. The underlying type is child 0 and
// is invalid when unwritten - the convention Var_decl already uses for a missing annotation, so
// the variants start at 1 and no consumer has to ask which kind the last child is.
TEST_CASE( "parser_parses_an_enum", "[parse]" )
{
    SECTION( "with an underlying type" )
    {
        const Parsed p( "enum Colour : u8 { Red, Green, Blue };" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id decl = find_first( p.ast(), p.root(), Node_kind::Enum_decl );

        REQUIRE( decl.is_valid() );
        REQUIRE( p.children( decl ).size() == 4 ); // the type, then three variants
        REQUIRE( p.kind( p.child( decl, 0 ) ) == Node_kind::Named_type );
        REQUIRE( p.kind( p.child( decl, 1 ) ) == Node_kind::Variant_decl );
        REQUIRE( p.kind( p.child( decl, 3 ) ) == Node_kind::Variant_decl );
    }

    SECTION( "without one, leaving child 0 invalid" )
    {
        const Parsed p( "enum Colour { Red, Green };" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id decl = find_first( p.ast(), p.root(), Node_kind::Enum_decl );

        REQUIRE( p.children( decl ).size() == 3 );
        REQUIRE_FALSE( p.child( decl, 0 ).is_valid() );
        REQUIRE( p.kind( p.child( decl, 1 ) ) == Node_kind::Variant_decl );
    }

    SECTION( "and a trailing comma is allowed" )
    {
        const Parsed p( "enum Colour { Red, Green, };" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );
        REQUIRE( p.children( find_first( p.ast(), p.root(), Node_kind::Enum_decl ) ).size() == 3 );
    }

    // D30 keeps the spelling recognised so the diagnostic can name the fix, which is D22's pattern
    // for `->`: a rejected spelling that lexes is worth more than one that does not.
    SECTION( "`enum class` names its replacement" )
    {
        const Parsed p( "enum class Colour { Red };" );

        INFO( p.errors() );
        REQUIRE( p.has_errors() );
        REQUIRE( p.errors().find( "write `enum`" ) != std::string::npos );
    }
}

// `Colour::Red` is a postfix operator binding as tightly as `.`, and the variant name goes in aux
// for the same reason a field's does: it resolves against the enum, not through the scope stack.
TEST_CASE( "parser_parses_a_path", "[parse]" )
{
    SECTION( "the variant is in aux and the qualifier is the child" )
    {
        const Parsed p( "i32 main() { return Colour::Red; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id path = find_first( p.ast(), p.root(), Node_kind::Path_expr );

        REQUIRE( path.is_valid() );
        REQUIRE( p.children( path ).size() == 1 );
        REQUIRE( p.kind( p.child( path, 0 ) ) == Node_kind::Name_expr );
    }

    // Tighter than every operator, so the comparison sees the whole path rather than the qualifier.
    SECTION( "and it binds tighter than a comparison" )
    {
        const Parsed p( "i32 main() { return Colour::Red == c; }" );

        INFO( p.errors() );
        REQUIRE_FALSE( p.has_errors() );

        const Node_id compare = find_first( p.ast(), p.root(), Node_kind::Binary_expr );

        REQUIRE( compare.is_valid() );
        REQUIRE( p.kind( p.child( compare, 0 ) ) == Node_kind::Path_expr );
    }
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
