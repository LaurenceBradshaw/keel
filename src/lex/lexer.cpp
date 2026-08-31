#include "lex/lexer.h"

#include <string>

namespace keel
{
namespace
{

// ASCII only, deliberately not <cctype>: those take an int and are UB for negative char values -
// every byte of a UTF-8 character in a string literal or comment - and are locale-dependent.

bool is_space( char c )
{
    return ( c == ' ' || c == '\t' || c == '\n' || c == '\r' );
}

bool is_digit( char c )
{
    return ( c >= '0' && c <= '9' );
}

bool is_hex_digit( char c )
{
    return is_digit( c ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
}

bool is_bin_digit( char c )
{
    return ( c == '0' || c == '1' );
}

bool is_ident_start( char c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_';
}

bool is_ident_continue( char c )
{
    return is_ident_start( c ) || is_digit( c );
}

class Scanner
{
public:
    Scanner( File_id file, const Source_manager& sm, Interner& interner, Diagnostics& diags );

    std::vector<Token> run();

private:
    bool at_end() const;

    // '\0' past the end, so scanners need no bounds checks. A real NUL in the file is rejected as
    // an invalid character.
    char peek( u32 ahead = 0 ) const;

    char advance();

    // Conditional consume. Always test the longest form first, or "<<=" lexes as "<" then "<=".
    bool match( char expected );

    Span span_from( u32 start ) const;
    void push( Token_kind kind, u32 start, Symbol_id symbol = Symbol_id {} );
    void error_at( Span span, std::string message, std::string help = {} );

    void skip_trivia();
    void skip_line_comment();

    // Block comments do NOT nest: "/* /* */" is valid C++ ending at the first "*/" (PLAN §5.1).
    void skip_block_comment( u32 start );

    // Scanners receive the offset of the token's first character, which run() has already consumed.

    void scan_identifier_or_keyword( u32 start );

    // Delimits only; nothing is converted, so overflow is not a lexer problem.
    void scan_number( u32 start );

    // seen_digit says whether a digit already precedes this run - true for decimal, where run()
    // consumed the first digit; false after an 0x/0b prefix.
    bool scan_digits( bool ( *is_valid )( char ), bool seen_digit );

    bool scan_fraction();

    // Clears ok and reports when the `e` is a broken exponent rather than the start of an
    // identifier.
    bool scan_exponent( bool& ok );

    // Trailing-identifier guard, then push. report is false when the literal already produced an
    // error, so one mistake gives one message.
    void finish_number( u32 start, Token_kind kind, bool report );

    // Validates the escape but does not decode it; the bytes are produced later, from the span.
    void scan_escape( u32 start );

    void scan_string( u32 start );

    // Exactly one character after escape processing: '' and 'ab' are errors.
    void scan_char( u32 start );

    // Must always make progress, or run() spins.
    void scan_punctuation( char c, u32 start );

    File_id            file_;
    Interner&          interner_;
    Diagnostics&       diags_;
    std::string_view   text_;
    u32                pos_ = 0;
    std::vector<Token> out_;
};

Scanner::Scanner( File_id file, const Source_manager& sm, Interner& interner, Diagnostics& diags )
    : file_( file ),
      interner_( interner ),
      diags_( diags ),
      text_( sm.file( file ).text )
{
}

std::vector<Token> Scanner::run()
{
    while( true )
    {
        skip_trivia();

        if( at_end() )
        {
            push( Token_kind::End_of_file, pos_ );
            return std::move( out_ );
        }

        const u32  start = pos_;
        const char c     = advance();

        if( is_ident_start( c ) )
        {
            scan_identifier_or_keyword( start );
        }
        else if( is_digit( c ) )
        {
            scan_number( start );
        }
        else if( c == '"' )
        {
            scan_string( start );
        }
        else if( c == '\'' )
        {
            scan_char( start );
        }
        else
        {
            scan_punctuation( c, start );
        }
    }
}

bool Scanner::at_end() const
{
    return pos_ >= text_.size();
}

char Scanner::peek( u32 ahead ) const
{
    if( pos_ + ahead < text_.size() )
    {
        return text_[pos_ + ahead];
    }

    return '\0';
}

char Scanner::advance()
{
    if( at_end() )
    {
        return '\0';
    }

    return text_[pos_++];
}

bool Scanner::match( char expected )
{
    if( peek() == expected )
    {
        ++pos_;
        return true;
    }

    return false;
}

Span Scanner::span_from( u32 start ) const
{
    return Span { file_, start, pos_ };
}

void Scanner::push( Token_kind kind, u32 start, Symbol_id symbol )
{
    out_.push_back( Token { kind, span_from( start ), symbol } );
}

void Scanner::error_at( Span span, std::string message, std::string help )
{
    diags_.error( span, std::move( message ), std::move( help ) );
}

void Scanner::skip_trivia()
{
    while( true )
    {
        char c = peek();
        if( is_space( c ) )
        {
            advance();
        }
        else if( c == '/' && peek( 1 ) == '/' )
        {
            skip_line_comment();
        }
        else if( c == '/' && peek( 1 ) == '*' )
        {
            skip_block_comment( pos_ );
        }
        else
        {
            break;
        }
    }
}

void Scanner::skip_line_comment()
{
    while( !at_end() && peek() != '\n' )
    {
        advance();
    }
}

void Scanner::skip_block_comment( u32 start )
{
    while( !at_end() )
    {
        if( peek() == '*' && peek( 1 ) == '/' )
        {
            advance();
            advance();
            return;
        }

        advance();
    }

    error_at( span_from( start ), "unterminated block comment" );
}

void Scanner::scan_identifier_or_keyword( u32 start )
{
    while( is_ident_continue( peek() ) )
    {
        advance();
    }

    std::string_view slice  = text_.substr( start, pos_ - start );
    Symbol_id        symbol = interner_.intern( slice );

    if( interner_.is_keyword( symbol ) )
    {
        push( Token_kind::Keyword, start, symbol );
    }
    else
    {
        push( Token_kind::Identifier, start, symbol );
    }
}

bool Scanner::scan_digits( bool ( *is_valid )( char ), bool seen_digit )
{
    while( true )
    {
        if( is_valid( peek() ) )
        {
            advance();
            seen_digit = true;
        }
        else if( peek() == '_' || peek() == '\'' )
        {
            const u32 separator = pos_;
            advance();

            // A separator is only legal between two digits. Report and carry on, so `1__0` gives
            // one error and still lexes as a single token.
            if( !seen_digit || !is_valid( peek() ) )
            {
                error_at( Span { file_, separator, pos_ }, "digit separator must be between digits" );
            }
        }
        else
        {
            return seen_digit;
        }
    }
}

bool Scanner::scan_fraction()
{
    if( peek() != '.' || !is_digit( peek( 1 ) ) )
    {
        return false;
    }

    advance();
    scan_digits( is_digit, false );
    return true;
}

bool Scanner::scan_exponent( bool& ok )
{
    if( peek() != 'e' && peek() != 'E' )
    {
        return false;
    }

    const u32 digit_at = ( peek( 1 ) == '+' || peek( 1 ) == '-' ) ? 2 : 1;

    if( is_digit( peek( digit_at ) ) )
    {
        advance();
        if( peek() == '+' || peek() == '-' )
        {
            advance();
        }

        scan_digits( is_digit, false );
        return true;
    }

    // `1else` is an identifier butted against a literal, not a broken exponent - leave it to the
    // trailing-identifier guard, which names the real problem.
    if( is_ident_continue( peek( 1 ) ) )
    {
        return false;
    }

    const u32 exponent = pos_;
    advance();
    if( peek() == '+' || peek() == '-' )
    {
        advance();
    }

    error_at( Span { file_, exponent, pos_ }, "exponent has no digits", "write at least one digit after `e`" );
    ok = false;
    return false;
}

void Scanner::finish_number( u32 start, Token_kind kind, bool report )
{
    if( is_ident_start( peek() ) )
    {
        const u32 trailing = pos_;
        while( is_ident_continue( peek() ) )
        {
            advance();
        }

        if( report )
        {
            error_at(
                Span { file_, trailing, pos_ },
                "unexpected `" + std::string( text_.substr( trailing, pos_ - trailing ) ) + "` after numeric literal",
                "literal suffixes are not supported"
            );
        }
    }

    push( kind, start );
}

void Scanner::scan_number( u32 start )
{
    // text_[start] is the first digit, already consumed by run().
    const bool leading_zero = text_[start] == '0';

    if( leading_zero && ( peek() == 'x' || peek() == 'X' ) )
    {
        advance();
        const bool digits = scan_digits( is_hex_digit, false );
        if( !digits )
        {
            error_at( span_from( start ), "hex literal has no digits", "write at least one digit after `0x`" );
        }

        finish_number( start, Token_kind::Int_literal, digits );
        return;
    }

    if( leading_zero && ( peek() == 'b' || peek() == 'B' ) )
    {
        advance();
        const bool digits = scan_digits( is_bin_digit, false );
        if( !digits )
        {
            error_at( span_from( start ), "binary literal has no digits", "write at least one digit after `0b`" );
        }

        finish_number( start, Token_kind::Int_literal, digits );
        return;
    }

    scan_digits( is_digit, true );

    bool ok = true;

    // C++ reads a leading zero as octal (010 == 8). Keel has no octal, so accepting it would
    // silently change the value of valid C++ (PLAN §6.3 D14).
    if( leading_zero && pos_ - start > 1 )
    {
        error_at(
            span_from( start ),
            "leading zeros are not allowed in numeric literals",
            "Keel has no octal literals; remove the leading zero"
        );
        ok = false;
    }

    const bool       fraction = scan_fraction();
    const bool       exponent = scan_exponent( ok );
    const Token_kind kind     = ( fraction || exponent ) ? Token_kind::Float_literal : Token_kind::Int_literal;

    finish_number( start, kind, ok );
}

void Scanner::scan_escape( u32 start )
{
    // A backslash at end of line or end of file is the caller's unterminated case, not ours.
    if( at_end() || peek() == '\n' )
    {
        return;
    }

    const char c = advance();

    switch( c )
    {
    case 'n':
    case 't':
    case 'r':
    case '0':
    case '\\':
    case '"':
    case '\'':
        return;

    case 'x':
        // Exactly two, unlike C++'s unbounded run, which silently overflows (PLAN §6.3 D13).
        if( is_hex_digit( peek() ) && is_hex_digit( peek( 1 ) ) )
        {
            advance();
            advance();
            return;
        }

        error_at( span_from( start ), "`\\x` needs exactly two hex digits", "for example `\\x41`" );
        return;

    default:
        error_at(
            span_from( start ),
            "unknown escape sequence `\\" + std::string( 1, c ) + "`",
            "valid escapes are \\n \\t \\r \\0 \\\\ \\\" \\' and \\xNN"
        );
        return;
    }
}

void Scanner::scan_string( u32 start )
{
    while( !at_end() && peek() != '\n' )
    {
        if( peek() == '"' )
        {
            advance();
            push( Token_kind::String_literal, start );
            return;
        }

        if( peek() == '\\' )
        {
            const u32 escape = pos_;
            advance();
            scan_escape( escape );
            continue;
        }

        advance();
    }

    // Point at the opening quote: the span to the end of the line is where it went wrong, not what.
    error_at( Span { file_, start, start + 1 }, "unterminated string literal", "add a closing `\"`" );
    push( Token_kind::String_literal, start );
}

void Scanner::scan_char( u32 start )
{
    u32 count = 0;

    while( !at_end() && peek() != '\n' )
    {
        if( peek() == '\'' )
        {
            advance();

            if( count == 0 )
            {
                error_at( span_from( start ), "empty character literal", "write one character between the quotes" );
            }
            else if( count > 1 )
            {
                error_at(
                    span_from( start ),
                    "character literal must contain exactly one character",
                    "use a string literal for more than one"
                );
            }

            push( Token_kind::Char_literal, start );
            return;
        }

        if( peek() == '\\' )
        {
            const u32 escape = pos_;
            advance();
            scan_escape( escape );
        }
        else
        {
            advance();
        }

        ++count;
    }

    error_at( Span { file_, start, start + 1 }, "unterminated character literal", "add a closing `'`" );
    push( Token_kind::Char_literal, start );
}

void Scanner::scan_punctuation( char c, u32 start )
{
    switch( c )
    {
    case '(':
        push( Token_kind::L_paren, start );
        return;
    case ')':
        push( Token_kind::R_paren, start );
        return;
    case '{':
        push( Token_kind::L_brace, start );
        return;
    case '}':
        push( Token_kind::R_brace, start );
        return;
    case '[':
        push( Token_kind::L_bracket, start );
        return;
    case ']':
        push( Token_kind::R_bracket, start );
        return;
    case ';':
        push( Token_kind::Semicolon, start );
        return;
    case ',':
        push( Token_kind::Comma, start );
        return;
    case '.':
        push( Token_kind::Dot, start );
        return;
    case '~':
        push( Token_kind::Tilde, start );
        return;
    case '?':
        push( Token_kind::Question, start );
        return;

    case ':':
        push( match( ':' ) ? Token_kind::Colon_colon : Token_kind::Colon, start );
        return;
    case '*':
        push( match( '=' ) ? Token_kind::Star_equal : Token_kind::Star, start );
        return;
    case '/':
        push( match( '=' ) ? Token_kind::Slash_equal : Token_kind::Slash, start );
        return;
    case '%':
        push( match( '=' ) ? Token_kind::Percent_equal : Token_kind::Percent, start );
        return;
    case '^':
        push( match( '=' ) ? Token_kind::Caret_equal : Token_kind::Caret, start );
        return;
    case '=':
        push( match( '=' ) ? Token_kind::Equal_equal : Token_kind::Equal, start );
        return;
    case '!':
        push( match( '=' ) ? Token_kind::Bang_equal : Token_kind::Bang, start );
        return;

    case '+':
        if( match( '+' ) )
        {
            push( Token_kind::Plus_plus, start );
            return;
        }
        push( match( '=' ) ? Token_kind::Plus_equal : Token_kind::Plus, start );
        return;

    case '-':
        if( match( '-' ) )
        {
            push( Token_kind::Minus_minus, start );
            return;
        }
        if( match( '>' ) )
        {
            push( Token_kind::Arrow, start );
            return;
        }
        push( match( '=' ) ? Token_kind::Minus_equal : Token_kind::Minus, start );
        return;

    case '&':
        if( match( '&' ) )
        {
            push( Token_kind::Amp_amp, start );
            return;
        }
        push( match( '=' ) ? Token_kind::Amp_equal : Token_kind::Amp, start );
        return;

    case '|':
        if( match( '|' ) )
        {
            push( Token_kind::Pipe_pipe, start );
            return;
        }
        push( match( '=' ) ? Token_kind::Pipe_equal : Token_kind::Pipe, start );
        return;

    case '<':
        if( match( '<' ) )
        {
            push( match( '=' ) ? Token_kind::Less_less_equal : Token_kind::Less_less, start );
            return;
        }
        push( match( '=' ) ? Token_kind::Less_equal : Token_kind::Less, start );
        return;

    case '>':
        if( match( '>' ) )
        {
            push( match( '=' ) ? Token_kind::Greater_greater_equal : Token_kind::Greater_greater, start );
            return;
        }
        push( match( '=' ) ? Token_kind::Greater_equal : Token_kind::Greater, start );
        return;

    case '#':
        error_at( span_from( start ), "Keel has no preprocessor", "use `import` instead of `#include`" );
        push( Token_kind::Unknown, start );
        return;

    default:
        break;
    }

    // Pasting from a web page is the usual source of these; naming the character beats printing an
    // invisible byte.
    if( c == '\xC2' && peek() == '\xA0' )
    {
        advance();
        error_at( span_from( start ), "unexpected non-breaking space (U+00A0)", "replace it with an ordinary space" );
        push( Token_kind::Unknown, start );
        return;
    }

    const unsigned char byte = static_cast<unsigned char>( c );

    if( byte >= 0x20 && byte < 0x7F )
    {
        error_at( span_from( start ), "unexpected character `" + std::string( 1, c ) + "`" );
    }
    else
    {
        static constexpr char k_hex[] = "0123456789ABCDEF";
        const std::string     hex     = std::string( 1, k_hex[byte >> 4] ) + k_hex[byte & 0x0F];
        error_at( span_from( start ), "unexpected byte 0x" + hex );
    }

    push( Token_kind::Unknown, start );
}

} // namespace

std::vector<Token> lex( File_id file, const Source_manager& sm, Interner& interner, Diagnostics& diags )
{
    return Scanner( file, sm, interner, diags ).run();
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <sstream>

namespace keel
{
namespace
{

// Scanner is private, so the tests drive the public entry point. For a lexer that loses nothing:
// every behaviour is reachable as string in, tokens out.
class Lexed
{
public:
    explicit Lexed( std::string_view source )
    {
        file_   = sm_.add_file( "t.kl", std::string( source ) );
        tokens_ = lex( file_, sm_, interner_, diags_ );
    }

    // Excludes the End_of_file token.
    std::size_t count() const
    {
        return tokens_.size() - 1;
    }

    Token_kind kind( std::size_t i ) const
    {
        return tokens_[i].kind;
    }
    std::string_view text( std::size_t i ) const
    {
        return sm_.text( tokens_[i].span );
    }
    Span span( std::size_t i ) const
    {
        return tokens_[i].span;
    }
    Symbol_id symbol( std::size_t i ) const
    {
        return tokens_[i].symbol;
    }
    Keyword keyword( std::size_t i ) const
    {
        return tokens_[i].keyword();
    }

    const Token& eof() const
    {
        return tokens_.back();
    }

    bool has_errors() const
    {
        return diags_.has_errors();
    }
    std::size_t error_count() const
    {
        return diags_.error_count();
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags_.render( sm_, out );
        return out.str();
    }

    std::vector<Token_kind> kinds() const
    {
        std::vector<Token_kind> out;
        for( std::size_t i = 0; i < count(); ++i )
        {
            out.push_back( tokens_[i].kind );
        }
        return out;
    }

private:
    Source_manager     sm_;
    Interner           interner_;
    Diagnostics        diags_;
    File_id            file_;
    std::vector<Token> tokens_;
};

// Lexes a single token and returns its kind, requiring that it really is one token.
Token_kind only_kind( std::string_view source )
{
    const Lexed lexed( source );
    REQUIRE( lexed.count() == 1 );
    return lexed.kind( 0 );
}

} // namespace

TEST_CASE( "lexer_empty_input_is_just_eof", "[lex]" )
{
    const Lexed lexed( "" );

    REQUIRE( lexed.count() == 0 );
    REQUIRE( lexed.eof().kind == Token_kind::End_of_file );
    REQUIRE( lexed.eof().span.is_empty() );
    REQUIRE( lexed.eof().span.start == 0 );
    REQUIRE_FALSE( lexed.has_errors() );
}

// "unexpected end of input" needs somewhere to point.
TEST_CASE( "lexer_eof_token_sits_at_end_of_text", "[lex]" )
{
    const Lexed lexed( "i32 x;" );

    REQUIRE( lexed.eof().kind == Token_kind::End_of_file );
    REQUIRE( lexed.eof().span.start == 6 );
    REQUIRE( lexed.eof().span.is_empty() );
}

TEST_CASE( "lexer_identifiers_and_keywords", "[lex]" )
{
    const Lexed lexed( "widget if iff While" );

    REQUIRE( lexed.count() == 4 );
    REQUIRE( lexed.kind( 0 ) == Token_kind::Identifier );
    REQUIRE( lexed.kind( 1 ) == Token_kind::Keyword );
    REQUIRE( lexed.keyword( 1 ) == Keyword::If );
    REQUIRE( lexed.kind( 2 ) == Token_kind::Identifier ); // iff is not if
    REQUIRE( lexed.kind( 3 ) == Token_kind::Identifier ); // case sensitive
    REQUIRE( lexed.text( 0 ) == "widget" );
}

TEST_CASE( "lexer_identifier_charset", "[lex]" )
{
    const Lexed lexed( "_ _a a1 _1_a" );

    REQUIRE( lexed.count() == 4 );
    for( std::size_t i = 0; i < 4; ++i )
    {
        INFO( "token " << i );
        REQUIRE( lexed.kind( i ) == Token_kind::Identifier );
    }
    REQUIRE( lexed.text( 3 ) == "_1_a" );
}

// PLAN §6.3 D1: the lexer must recognise these to say "use `i32` instead".
TEST_CASE( "lexer_reserved_cpp_words_are_keywords", "[lex]" )
{
    const Lexed lexed( "int long char unsigned new delete" );

    REQUIRE( lexed.count() == 6 );
    for( std::size_t i = 0; i < 6; ++i )
    {
        INFO( "token " << i << " = " << lexed.text( i ) );
        REQUIRE( lexed.kind( i ) == Token_kind::Keyword );
    }
    REQUIRE( lexed.keyword( 0 ) == Keyword::Int );
}

TEST_CASE( "lexer_only_identifiers_and_keywords_carry_symbols", "[lex]" )
{
    const Lexed lexed( "x if 42 \"s\" ;" );

    REQUIRE( lexed.symbol( 0 ).is_valid() );
    REQUIRE( lexed.symbol( 1 ).is_valid() );
    REQUIRE_FALSE( lexed.symbol( 2 ).is_valid() );
    REQUIRE_FALSE( lexed.symbol( 3 ).is_valid() );
    REQUIRE_FALSE( lexed.symbol( 4 ).is_valid() );
}

TEST_CASE( "lexer_skips_whitespace_including_newlines", "[lex]" )
{
    const Lexed lexed( "  a\n\tb\r\n  c  " );

    REQUIRE( lexed.count() == 3 );
    REQUIRE( lexed.text( 0 ) == "a" );
    REQUIRE( lexed.text( 1 ) == "b" );
    REQUIRE( lexed.text( 2 ) == "c" );
    REQUIRE_FALSE( lexed.has_errors() );
}

TEST_CASE( "lexer_line_comments", "[lex]" )
{
    const Lexed lexed( "a // this is ignored\nb" );

    REQUIRE( lexed.count() == 2 );
    REQUIRE( lexed.text( 0 ) == "a" );
    REQUIRE( lexed.text( 1 ) == "b" );
}

TEST_CASE( "lexer_line_comment_at_end_of_file", "[lex]" )
{
    const Lexed lexed( "a // trailing" );

    REQUIRE( lexed.count() == 1 );
    REQUIRE_FALSE( lexed.has_errors() );
}

TEST_CASE( "lexer_block_comments", "[lex]" )
{
    const Lexed lexed( "a /* ignored\n   across lines */ b" );

    REQUIRE( lexed.count() == 2 );
    REQUIRE( lexed.text( 1 ) == "b" );
}

// PLAN §5.1: "/* /* */" is valid C++ ending at the first "*/", so nesting would change its meaning.
TEST_CASE( "lexer_block_comments_do_not_nest", "[lex]" )
{
    const Lexed lexed( "/* /* */ a" );

    REQUIRE( lexed.count() == 1 );
    REQUIRE( lexed.text( 0 ) == "a" );
    REQUIRE_FALSE( lexed.has_errors() );
}

TEST_CASE( "lexer_unterminated_block_comment", "[lex]" )
{
    const Lexed lexed( "a /* never closed" );

    REQUIRE( lexed.has_errors() );
    REQUIRE( lexed.rendered().find( "unterminated block comment" ) != std::string::npos );
}

TEST_CASE( "lexer_adjacent_trivia", "[lex]" )
{
    const Lexed lexed( " /*x*/ // y\n /*z*/ a" );

    REQUIRE( lexed.count() == 1 );
    REQUIRE( lexed.text( 0 ) == "a" );
}

TEST_CASE( "lexer_maximal_munch", "[lex]" )
{
    struct Case
    {
        const char* source;
        Token_kind  kind;
    };

    static const Case cases[] = {
        { "<", Token_kind::Less },
        { "<=", Token_kind::Less_equal },
        { "<<", Token_kind::Less_less },
        { "<<=", Token_kind::Less_less_equal },
        { ">", Token_kind::Greater },
        { ">=", Token_kind::Greater_equal },
        { ">>", Token_kind::Greater_greater },
        { ">>=", Token_kind::Greater_greater_equal },
        { "+", Token_kind::Plus },
        { "++", Token_kind::Plus_plus },
        { "+=", Token_kind::Plus_equal },
        { "-", Token_kind::Minus },
        { "--", Token_kind::Minus_minus },
        { "->", Token_kind::Arrow },
        { "-=", Token_kind::Minus_equal },
        { "&", Token_kind::Amp },
        { "&&", Token_kind::Amp_amp },
        { "&=", Token_kind::Amp_equal },
        { "|", Token_kind::Pipe },
        { "||", Token_kind::Pipe_pipe },
        { "|=", Token_kind::Pipe_equal },
        { "=", Token_kind::Equal },
        { "==", Token_kind::Equal_equal },
        { "!", Token_kind::Bang },
        { "!=", Token_kind::Bang_equal },
        { ":", Token_kind::Colon },
        { "::", Token_kind::Colon_colon },
        { "*", Token_kind::Star },
        { "*=", Token_kind::Star_equal },
        { "/", Token_kind::Slash },
        { "/=", Token_kind::Slash_equal },
        { "%", Token_kind::Percent },
        { "%=", Token_kind::Percent_equal },
        { "^", Token_kind::Caret },
        { "^=", Token_kind::Caret_equal },
        { "~", Token_kind::Tilde },
        { "?", Token_kind::Question },
        { ".", Token_kind::Dot },
        { "(", Token_kind::L_paren },
        { ")", Token_kind::R_paren },
        { "{", Token_kind::L_brace },
        { "}", Token_kind::R_brace },
        { "[", Token_kind::L_bracket },
        { "]", Token_kind::R_bracket },
        { ";", Token_kind::Semicolon },
        { ",", Token_kind::Comma },
    };

    for( const Case& c : cases )
    {
        INFO( "source '" << c.source << "' expecting " << token_kind_name( c.kind ) );
        REQUIRE( only_kind( c.source ) == c.kind );
    }
}

// The ladder must not consume across token boundaries.
TEST_CASE( "lexer_operator_sequences", "[lex]" )
{
    const Lexed lexed( "<<<" );

    REQUIRE( lexed.count() == 2 );
    REQUIRE( lexed.kind( 0 ) == Token_kind::Less_less );
    REQUIRE( lexed.kind( 1 ) == Token_kind::Less );
}

TEST_CASE( "lexer_generic_close_is_two_greaters", "[lex]" )
{
    // Vector<Vector<i32>> - the parser splits these; the lexer must not invent a different token.
    const Lexed lexed( "a>>b" );

    REQUIRE( lexed.kind( 1 ) == Token_kind::Greater_greater );
}

TEST_CASE( "lexer_numbers", "[lex]" )
{
    REQUIRE( only_kind( "0" ) == Token_kind::Int_literal );
    REQUIRE( only_kind( "42" ) == Token_kind::Int_literal );
    REQUIRE( only_kind( "0xFF" ) == Token_kind::Int_literal );
    REQUIRE( only_kind( "0b1010" ) == Token_kind::Int_literal );
    REQUIRE( only_kind( "1_000" ) == Token_kind::Int_literal );
    REQUIRE( only_kind( "1'000'000" ) == Token_kind::Int_literal );
    REQUIRE( only_kind( "1.5" ) == Token_kind::Float_literal );
    REQUIRE( only_kind( "1e10" ) == Token_kind::Float_literal );
    REQUIRE( only_kind( "1.5e-3" ) == Token_kind::Float_literal );
    REQUIRE( only_kind( "2E+8" ) == Token_kind::Float_literal );
}

TEST_CASE( "lexer_number_boundaries", "[lex]" )
{
    SECTION( "dot followed by a non-digit is member access" )
    {
        const Lexed lexed( "1.method" );
        REQUIRE( lexed.count() == 3 );
        REQUIRE( lexed.kind( 0 ) == Token_kind::Int_literal );
        REQUIRE( lexed.kind( 1 ) == Token_kind::Dot );
        REQUIRE( lexed.kind( 2 ) == Token_kind::Identifier );
    }

    SECTION( "range-looking input keeps both dots" )
    {
        const Lexed lexed( "1..2" );
        REQUIRE( lexed.count() == 4 );
    }
}

TEST_CASE( "lexer_number_errors", "[lex]" )
{
    struct Case
    {
        const char* source;
        const char* message;
    };

    static const Case cases[] = {
        { "1_", "digit separator must be between digits" },
        { "1__0", "digit separator must be between digits" },
        { "0b_1", "digit separator must be between digits" },
        { "0x", "hex literal has no digits" },
        { "0b", "binary literal has no digits" },
        { "42u", "after numeric literal" },
        { "1e", "exponent has no digits" },
        { "1e+", "exponent has no digits" },
        { "007", "leading zeros are not allowed" },
    };

    for( const Case& c : cases )
    {
        const Lexed lexed( c.source );
        INFO( "source '" << c.source << "' rendered:\n" << lexed.rendered() );
        REQUIRE( lexed.error_count() == 1 );
        REQUIRE( lexed.rendered().find( c.message ) != std::string::npos );
        REQUIRE( lexed.count() == 1 ); // still one token, for recovery
    }
}

// `1else` is an identifier butted against a literal, not a broken exponent.
TEST_CASE( "lexer_trailing_identifier_is_not_a_broken_exponent", "[lex]" )
{
    const Lexed lexed( "1else" );

    REQUIRE( lexed.rendered().find( "unexpected `else` after numeric literal" ) != std::string::npos );
}

TEST_CASE( "lexer_strings", "[lex]" )
{
    REQUIRE( only_kind( "\"\"" ) == Token_kind::String_literal );
    REQUIRE( only_kind( "\"hello\"" ) == Token_kind::String_literal );

    const Lexed lexed( "\"a\\\"b\"" );
    REQUIRE( lexed.count() == 1 );
    REQUIRE( lexed.text( 0 ) == "\"a\\\"b\"" ); // the escaped quote did not terminate it
    REQUIRE_FALSE( lexed.has_errors() );
}

TEST_CASE( "lexer_string_escapes", "[lex]" )
{
    const Lexed lexed( "\"\\n\\t\\r\\0\\\\\\\"\\'\\x41\"" );

    REQUIRE( lexed.count() == 1 );
    INFO( lexed.rendered() );
    REQUIRE_FALSE( lexed.has_errors() );
}

TEST_CASE( "lexer_string_escape_errors", "[lex]" )
{
    SECTION( "unknown escape" )
    {
        const Lexed lexed( "\"\\q\"" );
        REQUIRE( lexed.rendered().find( "unknown escape sequence" ) != std::string::npos );
        REQUIRE( lexed.count() == 1 );
    }

    SECTION( "hex escape needs exactly two digits" )
    {
        const Lexed lexed( "\"\\x4\"" );
        REQUIRE( lexed.rendered().find( "exactly two hex digits" ) != std::string::npos );
    }

    SECTION( "hex escape rejects non-hex" )
    {
        const Lexed lexed( "\"\\xZZ\"" );
        REQUIRE( lexed.rendered().find( "exactly two hex digits" ) != std::string::npos );
    }
}

// One missing quote must not swallow the rest of the file.
TEST_CASE( "lexer_unterminated_string_stops_at_end_of_line", "[lex]" )
{
    const Lexed lexed( "\"oops\nx" );

    REQUIRE( lexed.rendered().find( "unterminated string literal" ) != std::string::npos );
    REQUIRE( lexed.count() == 2 );
    REQUIRE( lexed.kind( 0 ) == Token_kind::String_literal );
    REQUIRE( lexed.kind( 1 ) == Token_kind::Identifier );
    REQUIRE( lexed.text( 1 ) == "x" );
}

TEST_CASE( "lexer_unterminated_string_at_eof", "[lex]" )
{
    const Lexed lexed( "\"oops" );

    REQUIRE( lexed.rendered().find( "unterminated string literal" ) != std::string::npos );
    REQUIRE( lexed.count() == 1 );
}

TEST_CASE( "lexer_char_literals", "[lex]" )
{
    SECTION( "valid" )
    {
        REQUIRE( only_kind( "'a'" ) == Token_kind::Char_literal );
        REQUIRE( only_kind( "'\\n'" ) == Token_kind::Char_literal );
        REQUIRE( only_kind( "'\\''" ) == Token_kind::Char_literal );
    }

    SECTION( "empty is an error" )
    {
        const Lexed lexed( "''" );
        REQUIRE( lexed.rendered().find( "empty character literal" ) != std::string::npos );
    }

    SECTION( "multi-character is an error" )
    {
        const Lexed lexed( "'ab'" );
        REQUIRE( lexed.rendered().find( "exactly one character" ) != std::string::npos );
    }

    SECTION( "unterminated" )
    {
        const Lexed lexed( "'a" );
        REQUIRE( lexed.rendered().find( "unterminated character literal" ) != std::string::npos );
    }
}

// PLAN §6.3 D8.
TEST_CASE( "lexer_hash_reports_no_preprocessor", "[lex]" )
{
    const Lexed lexed( "#include <stdio.h>" );

    REQUIRE( lexed.has_errors() );
    REQUIRE( lexed.rendered().find( "Keel has no preprocessor" ) != std::string::npos );
    REQUIRE( lexed.rendered().find( "use `import`" ) != std::string::npos );
    REQUIRE( lexed.kind( 0 ) == Token_kind::Unknown );
}

TEST_CASE( "lexer_unexpected_characters", "[lex]" )
{
    SECTION( "printable" )
    {
        const Lexed lexed( "@" );
        REQUIRE( lexed.rendered().find( "unexpected character `@`" ) != std::string::npos );
        REQUIRE( lexed.kind( 0 ) == Token_kind::Unknown );
    }

    SECTION( "non-printable prints the byte" )
    {
        const Lexed lexed( std::string_view( "\x01", 1 ) );
        REQUIRE( lexed.rendered().find( "unexpected byte 0x01" ) != std::string::npos );
    }

    SECTION( "embedded NUL is not treated as end of file" )
    {
        const Lexed lexed( std::string_view( "a\0b", 3 ) );
        REQUIRE( lexed.count() == 3 );
        REQUIRE( lexed.kind( 1 ) == Token_kind::Unknown );
    }

    SECTION( "non-breaking space is named" )
    {
        const Lexed lexed( "a\xC2\xA0"
                           "b" );
        REQUIRE( lexed.rendered().find( "non-breaking space" ) != std::string::npos );
    }
}

// Recovery: a bad character must not stop the stream or spin the loop.
TEST_CASE( "lexer_recovers_and_keeps_going", "[lex]" )
{
    const Lexed lexed( "a @ b @ c" );

    REQUIRE( lexed.count() == 5 );
    REQUIRE( lexed.error_count() == 2 );
    REQUIRE( lexed.kind( 4 ) == Token_kind::Identifier );
    REQUIRE( lexed.text( 4 ) == "c" );
}

TEST_CASE( "lexer_spans_cover_exactly_their_tokens", "[lex]" )
{
    const Lexed lexed( "i32 add( a );" );

    for( std::size_t i = 0; i < lexed.count(); ++i )
    {
        INFO( "token " << i << " = " << token_kind_name( lexed.kind( i ) ) );
        REQUIRE_FALSE( lexed.span( i ).is_empty() );
    }

    REQUIRE( lexed.text( 0 ) == "i32" );
    REQUIRE( lexed.text( 1 ) == "add" );
    REQUIRE( lexed.text( 2 ) == "(" );
    REQUIRE( lexed.text( 3 ) == "a" );
    REQUIRE( lexed.text( 4 ) == ")" );
    REQUIRE( lexed.text( 5 ) == ";" );
}

TEST_CASE( "lexer_whole_function", "[lex]" )
{
    const Lexed lexed( "i32 add( i32 a, i32 b )\n{\n    return a + b;\n}\n" );

    const std::vector<Token_kind> expected = {
        Token_kind::Identifier,
        Token_kind::Identifier,
        Token_kind::L_paren,
        Token_kind::Identifier,
        Token_kind::Identifier,
        Token_kind::Comma,
        Token_kind::Identifier,
        Token_kind::Identifier,
        Token_kind::R_paren,
        Token_kind::L_brace,
        Token_kind::Keyword,
        Token_kind::Identifier,
        Token_kind::Plus,
        Token_kind::Identifier,
        Token_kind::Semicolon,
        Token_kind::R_brace,
    };

    REQUIRE_FALSE( lexed.has_errors() );
    REQUIRE( lexed.kinds() == expected );
    REQUIRE( lexed.keyword( 10 ) == Keyword::Return );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
