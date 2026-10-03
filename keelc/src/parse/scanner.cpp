// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parse/scanner.h"
#include <algorithm>
#include <cassert>

namespace keel
{

Scanner::Scanner( std::span<const Token> tokens )
    : tokens_( tokens )
{
}

Head_scan Scanner::member_head( u32 at, Symbol_id enclosing )
{
    return scan_head( at, enclosing, Constructor_names::Any );
}

Member_chunk Scanner::next_member( u32 at, Symbol_id enclosing )
{
    cursor_ = at;

    if( check( Token_kind::R_brace ) || at_end() )
    {
        return Member_chunk { .dropped_begin = at, .dropped_end = at, .failure = Scan_failure {}, .head = std::nullopt };
    }

    const Head_scan first = member_head( at, enclosing );

    if( first.head.has_value() )
    {
        return Member_chunk { .dropped_begin = at, .dropped_end = at, .failure = Scan_failure {}, .head = first.head };
    }

    // Missing `;` repair
    if( first.failure.wanted == Wanted::Token && first.failure.token == Token_kind::Semicolon )
    {
        const Head_scan next = scan_head( first.failure.at, enclosing, Constructor_names::Enclosing_only );

        if( next.head.has_value() )
        {
            cursor_          = at;
            Member_kind kind = Member_kind::Field;

            // Match public, or failing that private, but never both.
            if( !match_keyword( Keyword::Public ) )
            {
                match_keyword( Keyword::Private );
            }

            if( match_keyword( Keyword::Static ) )
            {
                kind = Member_kind::Static_var;
            }

            return Member_chunk {
                .dropped_begin = at,
                .dropped_end   = at,
                .failure       = Scan_failure {},
                .head          = Member_head { .kind = kind, .start = at, .commit = first.failure.at }
            };
        }
    }

    // Drop then retry; to catch garbage at the beginning of something valid
    cursor_ = at;
    if( check( Token_kind::L_brace ) )
    {
        skip_braces();
    }
    else
    {
        advance();
    }

    u32 q = cursor_;
    while( true )
    {
        cursor_ = q;

        if( check( Token_kind::R_brace ) || at_end() )
        {
            break;
        }

        if( check( Token_kind::L_brace ) )
        {
            skip_braces();
            q = cursor_;
            continue;
        }

        const Head_scan retry = member_head( q, enclosing );

        if( !retry.head.has_value() )
        {
            q++;
            continue;
        }

        return Member_chunk { .dropped_begin = at, .dropped_end = q, .failure = first.failure, .head = retry.head };
    }

    return Member_chunk { .dropped_begin = at, .dropped_end = q, .failure = first.failure, .head = std::nullopt };
}

Head_scan Scanner::scan_head( u32 at, Symbol_id enclosing, Constructor_names names )
{
    auto make_head = [this]( Member_kind kind, u32 start ) -> Head_scan {
        return Head_scan {
            .head = Member_head { .kind = kind, .start = start, .commit = cursor_ }, .failure = Scan_failure {}
        };
    };

    auto make_failure = [this]() -> Head_scan { return Head_scan { .head = std::nullopt, .failure = failure_ }; };

    const u32 start = at;

    cursor_       = at;
    owed_greater_ = 0;
    failure_      = Scan_failure {};

    // Match public, or failing that private, but never both.
    if( !match_keyword( Keyword::Public ) )
    {
        match_keyword( Keyword::Private );
    }

    const bool is_static = match_keyword( Keyword::Static );

    if( match( Token_kind::Tilde ) )
    {
        if( !want_name() || !skip_parens() )
        {
            return make_failure();
        }

        if( check( Token_kind::L_brace ) )
        {
            return make_head( Member_kind::Destructor, start );
        }

        fail( Wanted::Token, Token_kind::L_brace );
        return make_failure();
    }

    if( check( Token_kind::Identifier ) && peek( 1 ).kind == Token_kind::L_paren &&
        ( names == Constructor_names::Any || peek().symbol == enclosing ) )
    {
        advance(); // identifier

        if( !skip_parens() )
        {
            return make_failure();
        }

        if( check( Token_kind::L_brace ) )
        {
            return make_head( Member_kind::Constructor, start );
        }

        fail( Wanted::Token, Token_kind::L_brace );
        return make_failure();
    }

    if( cursor_ == start && !( check( Token_kind::Identifier ) || check_keyword( Keyword::Const ) ||
                               check_keyword( Keyword::Fn ) || check_keyword( Keyword::Field ) || at_mode_keyword() ) )
    {
        fail( Wanted::Member );
        return make_failure();
    }

    const u32  type_start = cursor_;
    const bool moded      = at_mode_keyword() || ( check_keyword( Keyword::Const ) && peek( 1 ).kind == Token_kind::Keyword &&
                                              peek( 1 ).keyword() == Keyword::Ref );

    if( !scan_type_with_mode() )
    {
        return make_failure();
    }

    if( check( Token_kind::Identifier ) && peek().symbol == enclosing && peek( 1 ).kind == Token_kind::L_paren )
    {
        fail( Wanted::Name );
        return make_failure();
    }

    if( !want_name() )
    {
        return make_failure();
    }

    if( check( Token_kind::L_paren ) )
    {
        if( !skip_parens() )
        {
            return make_failure();
        }

        match_keyword( Keyword::Const ); // optional

        if( check( Token_kind::L_brace ) )
        {
            // Whether it was a static method or not does not matter to the scanner, `static` was already consumed.
            // The parser will figure it out.
            return make_head( Member_kind::Method, start );
        }

        fail( Wanted::Token, Token_kind::L_brace );
        return make_failure();
    }

    if( is_static )
    {
        if( check( Token_kind::Equal ) || check( Token_kind::Semicolon ) )
        {
            return make_head( Member_kind::Static_var, start );
        }

        fail( Wanted::Token, Token_kind::Semicolon );
        return make_failure();
    }

    if( moded )
    {
        failure_ = Scan_failure { .at = type_start, .wanted = Wanted::Type };
        return make_failure();
    }
    else
    {
        if( check( Token_kind::Semicolon ) )
        {
            return make_head( Member_kind::Field, start );
        }

        fail( Wanted::Token, Token_kind::Semicolon );
        return make_failure();
    }
}

const Token& Scanner::peek( u32 ahead ) const
{
    assert( !tokens_.empty() );
    u32 tokens_end = narrow_cast<u32>( tokens_.size() - 1 );
    return tokens_[std::min( cursor_ + ahead, tokens_end )];
}

bool Scanner::at_end() const
{
    return peek().kind == Token_kind::End_of_file;
}

bool Scanner::check( Token_kind kind ) const
{
    return peek().kind == kind;
}

bool Scanner::check_keyword( Keyword keyword ) const
{
    return peek().kind == Token_kind::Keyword && peek().keyword() == keyword;
}

bool Scanner::at_mode_keyword() const
{
    return check_keyword( Keyword::Move ) || check_keyword( Keyword::Ref ) || check_keyword( Keyword::Out );
}

void Scanner::advance()
{
    if( !at_end() )
    {
        cursor_++;
    }
}

bool Scanner::match( Token_kind kind )
{
    if( check( kind ) )
    {
        advance();
        return true;
    }
    return false;
}

bool Scanner::match_keyword( Keyword keyword )
{
    if( check_keyword( keyword ) )
    {
        advance();
        return true;
    }
    return false;
}

bool Scanner::fail( Wanted wanted, Token_kind token )
{
    failure_ = Scan_failure { .at = cursor_, .wanted = wanted, .token = token };
    return false;
}

bool Scanner::want( Token_kind kind )
{
    if( match( kind ) )
    {
        return true;
    }

    return fail( Wanted::Token, kind );
}

bool Scanner::want_name()
{
    // Follows parsers expect_name, which consumes these as the name they were meant to be.
    if( match( Token_kind::Identifier ) || match( Token_kind::Keyword ) || match( Token_kind::Digit_name ) ||
        match( Token_kind::Int_literal ) )
    {
        return true;
    }

    return fail( Wanted::Name );
}

bool Scanner::scan_type_with_mode()
{
    if( check_keyword( Keyword::Const ) && peek( 1 ).kind == Token_kind::Keyword && peek( 1 ).keyword() == Keyword::Ref )
    {
        advance(); // `const`
    }

    if( !at_mode_keyword() )
    {
        return scan_type();
    }

    advance();                       // `move`, `ref` or `out`
    match_keyword( Keyword::Const ); // optional; parser reports

    return scan_type();
}

bool Scanner::scan_type()
{
    match_keyword( Keyword::Const );

    if( check_keyword( Keyword::Fn ) || check_keyword( Keyword::Field ) )
    {
        bool is_fn = check_keyword( Keyword::Fn );
        advance(); // `fn` or `field`

        if( !want( Token_kind::L_paren ) )
        {
            return false;
        }

        if( is_fn )
        {
            if( !check( Token_kind::R_paren ) )
            {
                do
                {
                    if( !scan_type_with_mode() )
                    {
                        return false;
                    }
                } while( match( Token_kind::Comma ) );
            }
        }
        else // field
        {
            if( !scan_type_with_mode() )
            {
                return false;
            }
        }

        if( !want( Token_kind::R_paren ) )
        {
            return false;
        }

        if( !want( Token_kind::Arrow ) )
        {
            return false;
        }

        return scan_type_with_mode();
    }

    if( !match( Token_kind::Identifier ) )
    {
        return fail( Wanted::Type );
    }

    if( check( Token_kind::Colon_colon ) && peek( 1 ).kind == Token_kind::Identifier )
    {
        advance(); // `::`
        advance(); // identifier
    }

    if( match( Token_kind::Less ) )
    {
        if( !check( Token_kind::Greater ) && !check( Token_kind::Greater_greater ) )
        {
            do
            {
                if( !scan_type() )
                {
                    return false;
                }
            } while( match( Token_kind::Comma ) );
        }

        if( !scan_generic_close() )
        {
            return fail( Wanted::Token, Token_kind::Greater );
        }
    }

    while( true )
    {
        if( owed_greater_ > 0 )
        {
            break;
        }

        if( match_keyword( Keyword::Const ) )
        {
            continue;
        }

        if( check( Token_kind::L_bracket ) && peek( 1 ).kind == Token_kind::Star && peek( 2 ).kind == Token_kind::R_bracket )
        {
            advance(); // `[`
            advance(); // `*`
            advance(); // `]`
            continue;
        }

        if( match( Token_kind::Star ) || match( Token_kind::Amp ) )
        {
            continue;
        }

        break;
    }

    return true;
}

bool Scanner::scan_generic_close()
{
    if( owed_greater_ > 0 )
    {
        owed_greater_ -= 1;
        return true;
    }

    if( match( Token_kind::Greater ) )
    {
        return true;
    }

    if( check( Token_kind::Greater_greater ) )
    {
        advance();

        // Incremented rather than set: one pending close is all the current grammar can produce,
        // and a counter that cannot lose one is worth more than the assumption.
        owed_greater_ += 1;
        return true;
    }

    return false;
}

bool Scanner::skip_parens()
{
    if( !check( Token_kind::L_paren ) )
    {
        return fail( Wanted::Token, Token_kind::L_paren );
    }

    u32 depth = 0;
    while( !check( Token_kind::R_paren ) || depth > 0 )
    {
        if( at_end() || check( Token_kind::L_brace ) || check( Token_kind::R_brace ) || check( Token_kind::Semicolon ) )
        {
            return fail( Wanted::Token, Token_kind::R_paren );
        }

        if( check( Token_kind::L_paren ) )
        {
            depth++;
        }
        else if( check( Token_kind::R_paren ) )
        {
            depth--;
            if( depth == 0 )
            {
                advance(); // consume the `)`
                return true;
            }
        }

        advance();
    }

    return depth == 0;
}

void Scanner::skip_braces()
{
    if( !check( Token_kind::L_brace ) )
    {
        return;
    }

    u32 depth = 0;
    while( !check( Token_kind::R_brace ) || depth > 0 )
    {
        if( at_end() )
        {
            return;
        }

        if( check( Token_kind::L_brace ) )
        {
            depth++;
        }
        else if( check( Token_kind::R_brace ) )
        {
            depth--;
            if( depth == 0 )
            {
                advance(); // consume the `}`
                return;
            }
        }

        advance();
    }
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "common/diagnostics.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "lex/lexer.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace keel
{
namespace
{

// A class body's tokens, scanned as the body of `class C`.
class Scanned
{
public:
    explicit Scanned( std::string_view source )
    {
        file_   = sm_.add_file( "t.kl", std::string( source ) );
        tokens_ = lex( file_, sm_, interner_, literal_pool_, diags_ );
        name_   = interner_.intern( "C" );
    }

    Head_scan head( u32 at = 0 )
    {
        return Scanner( tokens_ ).member_head( at, name_ );
    }

    Member_chunk chunk( u32 at = 0 )
    {
        return Scanner( tokens_ ).next_member( at, name_ );
    }

    // Index of the nth token spelled `text`.
    u32 at( std::string_view text, u32 nth = 0 ) const
    {
        for( u32 i = 0; i < tokens_.size(); ++i )
        {
            if( sm_.text( tokens_[i].span ) == text && nth-- == 0 )
            {
                return i;
            }
        }

        FAIL( "no token `" << text << "`" );
        return 0;
    }

private:
    Source_manager     sm_;
    Interner           interner_;
    Literal_pool       literal_pool_;
    Diagnostics        diags_;
    File_id            file_;
    std::vector<Token> tokens_;
    Symbol_id          name_;
};

void require_head( std::string_view source, Member_kind kind, std::string_view commit )
{
    Scanned   s( source );
    Head_scan scan = s.head();

    INFO( source );
    REQUIRE( scan.head.has_value() );
    REQUIRE( scan.head->kind == kind );
    REQUIRE( scan.head->start == 0 );
    REQUIRE( scan.head->commit == s.at( commit ) );
}

void require_failure( std::string_view source, std::string_view at, Wanted wanted, u32 nth = 0 )
{
    Scanned   s( source );
    Head_scan scan = s.head();

    INFO( source );
    REQUIRE_FALSE( scan.head.has_value() );
    REQUIRE( scan.failure.at == s.at( at, nth ) );
    REQUIRE( scan.failure.wanted == wanted );
}

void require_failure( std::string_view source, std::string_view at, Token_kind token )
{
    Scanned   s( source );
    Head_scan scan = s.head();

    INFO( source );
    REQUIRE_FALSE( scan.head.has_value() );
    REQUIRE( scan.failure.at == s.at( at ) );
    REQUIRE( scan.failure.wanted == Wanted::Token );
    REQUIRE( scan.failure.token == token );
}

} // namespace

TEST_CASE( "scanner_recognises_each_member_head", "[scan]" )
{
    SECTION( "the five kinds, committed at their `{`, `=` or `;`" )
    {
        require_head( "i32 a;", Member_kind::Field, ";" );
        require_head( "i32 get() const { return 0; }", Member_kind::Method, "{" );
        require_head( "C( i32 v ) { }", Member_kind::Constructor, "{" );
        require_head( "~C() { }", Member_kind::Destructor, "{" );
        require_head( "static i32 n = 3;", Member_kind::Static_var, "=" );
        require_head( "static i32 n;", Member_kind::Static_var, ";" );
    }

    SECTION( "markers belong to the head" )
    {
        require_head( "public i32 a;", Member_kind::Field, ";" );
        require_head( "private static i32 make() { }", Member_kind::Method, "{" );
    }

    SECTION( "every type parse_type reads, including the ones it reads to refuse" )
    {
        require_head( "kl::Vec<i32> v;", Member_kind::Field, ";" );
        require_head( "Map<i32, Foo> m;", Member_kind::Field, ";" );
        require_head( "Box<Box<i32>> b;", Member_kind::Field, ";" );
        require_head( "Box<Box<i32>>* p;", Member_kind::Field, ";" );
        require_head( "i32[*] p;", Member_kind::Field, ";" );
        require_head( "i32* const p;", Member_kind::Field, ";" );
        require_head( "const i32 a;", Member_kind::Field, ";" );
        require_head( "i32& r;", Member_kind::Field, ";" );
        require_head( "i32 *p;", Member_kind::Field, ";" );
        require_head( "fn( i32, i32 ) -> i32 f;", Member_kind::Field, ";" );
    }

    SECTION( "a method's return type may carry a mode" )
    {
        require_head( "ref i32 at( u64 i ) { }", Member_kind::Method, "{" );
        require_head( "const ref Foo get() const { }", Member_kind::Method, "{" );
    }

    // What is inside is the parser's to report, and the body after it still gets checked.
    SECTION( "a parameter list is counted, not read" )
    {
        require_head( "i32 f( 42 + ) { }", Member_kind::Method, "{" );
        require_head( "C( ( i32 ) v ) { }", Member_kind::Constructor, "{" );
    }

    // Sema names the class it should have been; dropping it would lose that.
    SECTION( "a constructor with the wrong name is still a constructor" )
    {
        require_head( "D() { }", Member_kind::Constructor, "{" );
    }

    // expect_name consumes these as the name they were meant to be and says why each is not one.
    SECTION( "a keyword, a digit-led name or a number holds the name's place" )
    {
        require_head( "i32 if;", Member_kind::Field, ";" );
        require_head( "i32 move;", Member_kind::Field, ";" );
        require_head( "i32 3x;", Member_kind::Field, ";" );
        require_head( "i32 3;", Member_kind::Field, ";" );
        require_head( "i32 case() { }", Member_kind::Method, "{" );
        require_head( "static i32 for = 1;", Member_kind::Static_var, "=" );
        require_head( "~if() { }", Member_kind::Destructor, "{" );
    }
}

TEST_CASE( "scanner_says_where_and_why_a_head_fails", "[scan]" )
{
    SECTION( "nothing that can start a member" )
    {
        require_failure( "42;", "42", Wanted::Member );
        require_failure( "+ - *;", "+", Wanted::Member );
        require_failure( "return 1;", "return", Wanted::Member );
        require_failure( ";", ";", Wanted::Member );
    }

    SECTION( "a body where `{` should be" )
    {
        require_failure( "i32 f();", ";", Token_kind::L_brace );
        require_failure( "i32 f() const override { }", "override", Token_kind::L_brace );
        require_failure( "C() : a( 1 ) { }", ":", Token_kind::L_brace );
    }

    SECTION( "a field that goes on past its name" )
    {
        require_failure( "i32 b = 3;", "=", Token_kind::Semicolon );
        require_failure( "i32 b, c;", ",", Token_kind::Semicolon );
        require_failure( "i32 b[4];", "[", Token_kind::Semicolon );
    }

    SECTION( "a missing name or type" )
    {
        require_failure( "protected: i32 b;", ":", Wanted::Name );
        require_failure( "public public i32 b;", "public", Wanted::Type, 1 );
        require_failure( "Box<i32 b;", "b", Token_kind::Greater );
    }

    // A mode is a binding's, and a field is not one: parse_type refuses it.
    SECTION( "a field's type carries no mode" )
    {
        require_failure( "ref i32 a;", "ref", Wanted::Type );
    }

    // Or a stray word before a constructor reads as a method named after the class.
    SECTION( "the class's name before `(` is never a method's" )
    {
        require_failure( "i32 C() { }", "C", Wanted::Name );
    }
}

TEST_CASE( "scanner_chunks_a_class_body", "[scan]" )
{
    SECTION( "a member with nothing before it" )
    {
        Scanned            s( "i32 a; }" );
        const Member_chunk c = s.chunk();

        REQUIRE_FALSE( c.dropped() );
        REQUIRE( c.head.has_value() );
        REQUIRE( c.head->kind == Member_kind::Field );
        REQUIRE( c.head->start == 0 );
    }

    SECTION( "the body's end, and the file's" )
    {
        for( const std::string_view source : { "}", "" } )
        {
            Scanned            s( source );
            const Member_chunk c = s.chunk();

            INFO( source );
            REQUIRE_FALSE( c.dropped() );
            REQUIRE_FALSE( c.head.has_value() );
        }
    }

    SECTION( "junk, then the member after it" )
    {
        Scanned            s( "42; i32 n; }" );
        const Member_chunk c = s.chunk();

        REQUIRE( c.dropped_begin == 0 );
        REQUIRE( c.dropped_end == s.at( "i32" ) );
        REQUIRE( c.failure.at == 0 );
        REQUIRE( c.failure.wanted == Wanted::Member );
        REQUIRE( c.head.has_value() );
        REQUIRE( c.head->start == s.at( "i32" ) );
    }

    // The failure is the first one, at the start; the retry from the next token is what succeeds.
    SECTION( "a word before a member drops only the word" )
    {
        for( const std::string_view source : { "virtual i32 f() { } }", "mutable i32 b; }", "unsigned i32 b; }" } )
        {
            Scanned            s( source );
            const Member_chunk c = s.chunk();

            INFO( source );
            REQUIRE( c.dropped_begin == 0 );
            REQUIRE( c.dropped_end == 1 );
            REQUIRE( c.head.has_value() );
            REQUIRE( c.head->start == 1 );
        }

        Scanned s( "virtual i32 f() { } }" );
        REQUIRE( s.chunk().head->kind == Member_kind::Method );
        REQUIRE( s.chunk().failure.at == s.at( "f" ) );
        REQUIRE( s.chunk().failure.token == Token_kind::Semicolon );
    }

    SECTION( "a field missing its `;` before another member is kept" )
    {
        for( const std::string_view next :
             { "i32 n;", "i32 get() { }", "public i32 n;", "static i32 n;", "~C() { }", "C() { }" } )
        {
            Scanned            s( std::string( "i32 a\n" ) + std::string( next ) + " }" );
            const Member_chunk c = s.chunk();

            INFO( next );
            REQUIRE_FALSE( c.dropped() );
            REQUIRE( c.head.has_value() );
            REQUIRE( c.head->kind == Member_kind::Field );
            REQUIRE( c.head->commit == 2 );
        }
    }

    // `f() { }` heads a constructor only by the loose rule; this repair needs the class's name.
    SECTION( "the missing-`;` repair does not take any name for a constructor" )
    {
        Scanned            s( "virtual i32 f() { } }" );
        const Member_chunk c = s.chunk();

        REQUIRE( c.dropped() );
        REQUIRE( c.head->kind == Member_kind::Method );
    }

    SECTION( "a brace group is stepped over whole, even one the junk starts with" )
    {
        for( const std::string_view source :
             { "{ i32 z; } i32 n; }", "class D { i32 z; }; i32 n; }", "if( true ) { i32 z; } i32 n; }" } )
        {
            Scanned            s( source );
            const Member_chunk c = s.chunk();

            INFO( source );
            REQUIRE( c.dropped_begin == 0 );
            REQUIRE( c.head.has_value() );
            REQUIRE( c.head->start == s.at( "n" ) - 1 );
        }
    }

    SECTION( "a stray word before a constructor stays out of it" )
    {
        Scanned            s( "i32\nC( i32 v ) { } }" );
        const Member_chunk c = s.chunk();

        REQUIRE( c.dropped_end == 1 );
        REQUIRE( c.head->kind == Member_kind::Constructor );
    }

    SECTION( "junk running to the body's end" )
    {
        Scanned            s( "42 + }" );
        const Member_chunk c = s.chunk();

        REQUIRE( c.dropped_end == s.at( "}" ) );
        REQUIRE_FALSE( c.head.has_value() );
    }

    SECTION( "junk that recovers part-way along its line" )
    {
        Scanned s( "template<typename T> i32 f() { } }" );
        REQUIRE( s.chunk().head->start == s.at( "i32" ) );

        Scanned t( "protected: i32 b; }" );
        REQUIRE( t.chunk().head->start == t.at( "i32" ) );
    }

    SECTION( "scanning starts where it is told" )
    {
        Scanned            s( "i32 a; 42; i32 n; }" );
        const Member_chunk c = s.chunk( s.at( "42" ) );

        REQUIRE( c.dropped_begin == s.at( "42" ) );
        REQUIRE( c.head->start == s.at( "i32", 1 ) );
    }
}

} // namespace keel

#endif // ENABLE_UNIT_TESTS
