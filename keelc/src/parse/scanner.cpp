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

Head_scan Scanner::own_member_head( u32 at, Symbol_id enclosing )
{
    return scan_head( at, enclosing, Constructor_names::Enclosing_only );
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
    step_over_junk();

    u32 q = cursor_;
    while( true )
    {
        cursor_ = q;

        if( check( Token_kind::R_brace ) || at_end() )
        {
            break;
        }

        const Head_scan retry = scan_head( q, enclosing, Constructor_names::Enclosing_only );

        if( !retry.head.has_value() )
        {
            cursor_ = q;
            step_over_junk();
            q = cursor_;
            continue;
        }

        return Member_chunk { .dropped_begin = at, .dropped_end = q, .failure = first.failure, .head = retry.head };
    }

    return Member_chunk { .dropped_begin = at, .dropped_end = q, .failure = first.failure, .head = std::nullopt };
}

Declaration_scan Scanner::declaration_head( u32 at, Scan_site site )
{
    const auto make_head = [this]( Declaration_kind kind, u32 start ) -> Declaration_scan
    {
        return Declaration_scan {
            .head = Declaration_head { .kind = kind, .start = start, .commit = cursor_ }, .failure = Scan_failure {}
        };
    };
    const auto make_failure = [this]( std::optional<u32> name = std::nullopt ) -> Declaration_scan
    {
        failure_.name = name;
        return Declaration_scan { .head = std::nullopt, .failure = failure_ };
    };

    cursor_       = at;
    owed_greater_ = 0;
    failure_      = Scan_failure {};
    site_         = site;

    if( check_keyword( Keyword::Import ) )
    {
        return make_head( Declaration_kind::Import, at );
    }
    else if( check_keyword( Keyword::Struct ) || check_keyword( Keyword::Class ) )
    {
        return make_head( Declaration_kind::Aggregate, at );
    }
    else if( check_keyword( Keyword::Enum ) )
    {
        return make_head( Declaration_kind::Enum, at );
    }

    const bool is_extern = match_keyword( Keyword::Extern );

    if( !is_extern )
    {
        if( !( check( Token_kind::Identifier ) || check_keyword( Keyword::Const ) || check_keyword( Keyword::Fn ) ||
               check_keyword( Keyword::Field ) || at_mode_keyword() ) )
        {
            fail( Wanted::Declaration );
            return make_failure();
        }
    }

    if( !scan_type_with_mode() )
    {
        return make_failure();
    }

    const u32 name_at = cursor_;
    if( !want_name() )
    {
        return make_failure();
    }

    if( is_extern || check( Token_kind::L_paren ) || check( Token_kind::Less ) )
    {
        const std::optional<u32> name =
            tokens_[name_at].kind == Token_kind::Identifier ? std::optional( name_at ) : std::nullopt;
        if( check( Token_kind::Less ) )
        {
            if( !scan_type_params() )
            {
                return make_failure( name );
            }
        }

        if( !skip_parens() || !scan_where_clauses() )
        {
            return make_failure( name );
        }

        if( check( Token_kind::L_brace ) )
        {
            return make_head( Declaration_kind::Function, at );
        }

        if( is_extern && check( Token_kind::Semicolon ) )
        {
            return make_head( Declaration_kind::Function, at );
        }
        else if( is_extern )
        {
            fail( Wanted::Token, Token_kind::Semicolon );
            return make_failure( name );
        }

        fail( Wanted::Token, Token_kind::L_brace );
        return make_failure( name );
    }

    if( check( Token_kind::Equal ) || check( Token_kind::Semicolon ) )
    {
        return make_head( Declaration_kind::Variable, at );
    }

    fail( Wanted::Token, Token_kind::Semicolon );
    return make_failure();
}

Declaration_chunk Scanner::next_declaration( u32 at )
{
    cursor_ = at;

    if( at_end() )
    {
        return Declaration_chunk { .dropped_begin = at, .dropped_end = at, .failure = Scan_failure {}, .head = std::nullopt };
    }

    const Declaration_scan first = declaration_head( at );

    if( first.head.has_value() )
    {
        return Declaration_chunk { .dropped_begin = at, .dropped_end = at, .failure = Scan_failure {}, .head = first.head };
    }

    // Missing `;` repair
    if( first.failure.wanted == Wanted::Token && first.failure.token == Token_kind::Semicolon )
    {
        const Declaration_scan next = declaration_head( first.failure.at );

        if( next.head.has_value() )
        {
            cursor_               = at;
            Declaration_kind kind = Declaration_kind::Variable;

            if( tokens_[at].kind == Token_kind::Keyword && static_cast<Keyword>( tokens_[at].symbol.v ) == Keyword::Extern )
            {
                kind = Declaration_kind::Function;
            }

            return Declaration_chunk {
                .dropped_begin = at,
                .dropped_end   = at,
                .failure       = Scan_failure {},
                .head          = Declaration_head { .kind = kind, .start = at, .commit = first.failure.at }
            };
        }
    }

    // Drop and retry; to catch garbage at the beginning of something valid
    cursor_ = at;
    step_over_junk();

    u32 q = cursor_;
    while( true )
    {
        cursor_ = q;

        if( at_end() )
        {
            break;
        }

        const Declaration_scan retry = declaration_head( q );

        if( !retry.head.has_value() )
        {
            cursor_ = q;
            step_over_junk();
            q = cursor_;
            continue;
        }

        return Declaration_chunk { .dropped_begin = at, .dropped_end = q, .failure = first.failure, .head = retry.head };
    }

    return Declaration_chunk { .dropped_begin = at, .dropped_end = q, .failure = first.failure, .head = std::nullopt };
}

bool Scanner::looks_like_declaration( u32 at )
{
    cursor_       = at;
    owed_greater_ = 0;
    site_         = Scan_site::Statement;

    // `auto x = ...` is settled by its keyword; the caller checks that before asking.
    if( !check( Token_kind::Identifier ) && !check_keyword( Keyword::Const ) && !check_keyword( Keyword::Fn ) &&
        !check_keyword( Keyword::Field ) )
    {
        return false;
    }

    return scan_type_and_name();
}

bool Scanner::looks_like_binding( u32 at )
{
    cursor_       = at;
    owed_greater_ = 0;
    site_         = Scan_site::Statement;

    match_keyword( Keyword::Const ); // optional

    if( !at_mode_keyword() )
    {
        return false;
    }

    advance(); // `move`, `ref` or `out`
    return scan_type_and_name();
}

// From `at`: whether a `)` closes a group opened before it, and a `{` follows.
bool Scanner::head_closes( u32 at )
{
    u32 depth = 0;
    cursor_   = at;

    while( true )
    {
        if( at_end() || check( Token_kind::L_brace ) || check( Token_kind::R_brace ) )
        {
            return false;
        }

        switch( peek().kind )
        {
        case Token_kind::L_paren:
        case Token_kind::L_bracket:
            depth += 1;
            break;
        case Token_kind::R_paren:
        case Token_kind::R_bracket:
            if( depth == 0 )
            {
                return check( Token_kind::R_paren ) && peek( 1 ).kind == Token_kind::L_brace;
            }
            depth -= 1;
            break;
        default:
            break;
        }

        advance();
    }
}

// Shape alone, with no symbol table (L17). Taking the generic reading is safe: the comparison
// reading never type-checks (§12).
bool Scanner::looks_like_type_arguments( u32 at )
{
    cursor_       = at;
    owed_greater_ = 0;

    advance(); // the `<`

    u32 depth = 1;

    while( depth > 0 && !at_end() )
    {
        switch( peek().kind )
        {
        case Token_kind::Less:
            depth += 1;
            break;
        case Token_kind::Greater:
            depth -= 1;
            break;
        case Token_kind::Greater_greater:
            // One token closing two levels. A lone `>>` at depth 1 is a shift, not a close.
            if( depth < 2 )
            {
                return false;
            }
            depth -= 2;
            break;

        // A type argument holds no call, literal or operator: meeting one means a comparison.
        case Token_kind::Colon_colon:
        case Token_kind::Identifier:
        case Token_kind::Star:
        case Token_kind::Comma:
            break;

        default:
            return false;
        }

        advance();
    }

    return depth == 0 && ( check( Token_kind::Colon_colon ) || check( Token_kind::L_paren ) || check( Token_kind::Semicolon ) ||
                           check( Token_kind::Comma ) || check( Token_kind::R_paren ) || check( Token_kind::R_bracket ) ||
                           check( Token_kind::R_brace ) || check( Token_kind::Dot ) );
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
    site_         = Scan_site::File;

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

const Token& Scanner::previous() const
{
    assert( cursor_ > 0 );
    return tokens_[cursor_ - 1];
}

bool Scanner::peek_is_adjacent() const
{
    return cursor_ > 0 && peek().span.file == previous().span.file && peek().span.start == previous().span.end;
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
    if( at_name() )
    {
        advance();
        return true;
    }

    return fail( Wanted::Name );
}

bool Scanner::at_name() const
{
    // Follows parsers expect_name, which consumes these as the name they were meant to be.
    if( check( Token_kind::Identifier ) || check( Token_kind::Digit_name ) || check( Token_kind::Int_literal ) )
    {
        return true;
    }

    if( !check( Token_kind::Keyword ) )
    {
        return false;
    }

    if( site_ == Scan_site::File )
    {
        return true;
    }

    // A statement keyword starts its statement once what it needs follows; `;` follows a name too.
    switch( peek().keyword() )
    {
    case Keyword::If:
    case Keyword::While:
    case Keyword::For:
    case Keyword::Switch:
        return peek( 1 ).kind != Token_kind::L_paren;

    case Keyword::Unsafe:
        return peek( 1 ).kind != Token_kind::L_brace;

    case Keyword::Return:
        return peek( 1 ).kind == Token_kind::Equal || peek( 1 ).kind == Token_kind::Semicolon ||
               peek( 1 ).kind == Token_kind::Comma;

    default:
        return true;
    }
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

bool Scanner::scan_type_and_name()
{
    // `const i32 x`, `const ref T r`, `ref T r`.
    while( match_keyword( Keyword::Const ) )
    {
    }

    if( at_mode_keyword() )
    {
        advance();
    }

    // `fn( T, U ) -> R`, whose R may be another. Parens are counted, not read as types.
    while( check_keyword( Keyword::Fn ) || check_keyword( Keyword::Field ) )
    {
        advance();

        if( !check( Token_kind::L_paren ) )
        {
            return false;
        }

        u32 depth = 0;

        do
        {
            if( check( Token_kind::L_paren ) )
            {
                depth += 1;
            }
            else if( check( Token_kind::R_paren ) )
            {
                depth -= 1;
            }
            else if( check( Token_kind::Semicolon ) || check( Token_kind::R_brace ) || at_end() )
            {
                return false; // unbalanced - not a type
            }

            advance();
        } while( depth > 0 );

        // A missing arrow still scans as a declaration, so parse_type reports the arrow.
        if( !match( Token_kind::Arrow ) )
        {
            break;
        }

        // The return type's own `const` and mode.
        while( match_keyword( Keyword::Const ) )
        {
        }
        if( at_mode_keyword() )
        {
            advance();
        }
    }

    if( !match( Token_kind::Identifier ) )
    {
        return false;
    }

    // `kl::Point p`: no expression is a path followed by a name.
    if( check( Token_kind::Colon_colon ) && peek( 1 ).kind == Token_kind::Identifier )
    {
        advance();
        advance();
    }

    // What may follow a type's name before the declared name: `Point*`, `const T&`, `Vector<i32>`.
    while( true )
    {
        // Only an adjacent `*`/`&` is part of a type (D17), so `a * b;` stays an expression.
        if( ( check( Token_kind::Star ) || check( Token_kind::Amp ) ) && peek_is_adjacent() )
        {
            advance();
            continue;
        }

        if( check( Token_kind::L_bracket ) && peek( 1 ).kind == Token_kind::Star && peek( 2 ).kind == Token_kind::R_bracket )
        {
            advance();
            advance();
            advance();
            continue;
        }

        if( match_keyword( Keyword::Const ) )
        {
            continue;
        }

        // Type arguments, nesting counted: `>>` closes two levels.
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
                return false;
            }

            continue;
        }

        break;
    }

    // The name. A keyword, digit-led name or number holds its place, as for expect_name, so
    // `i32 out = 1;` reaches parse_var_decl and is reported there. At a statement's start,
    // a statement keyword followed by what it needs does not.
    if( !at_name() )
    {
        return false;
    }

    advance(); // the name
    return true;
}

bool Scanner::scan_type_params()
{
    advance(); // the `<`

    if( scan_generic_close() )
    {
        return true;
    }

    do
    {
        if( !want_name() )
        {
            return false;
        }

        match( Token_kind::Identifier ); // terse bound the parser reports
    } while( match( Token_kind::Comma ) );

    if( !scan_generic_close() )
    {
        return fail( Wanted::Token, Token_kind::Greater );
    }

    return true;
}

bool Scanner::scan_where_clauses()
{
    while( match_keyword( Keyword::Where ) )
    {
        if( !want_name() || !want( Token_kind::Colon ) )
        {
            return false;
        }

        bool separator = false;
        do
        {
            if( !match( Token_kind::Identifier ) )
            {
                return fail( Wanted::Name );
            }

            if( match( Token_kind::Amp ) )
            {
                separator = true;
            }
            else if( check( Token_kind::Comma ) && !( peek( 1 ).kind == Token_kind::Keyword &&
                                                      static_cast<Keyword>( peek( 1 ).symbol.v ) == Keyword::Where ) )
            {
                advance(); // `,`
                separator = true;
            }
            else if( check( Token_kind::Pipe ) )
            {
                advance(); // `|`
                separator = true;
            }
            else
            {
                separator = false;
            }
        } while( separator );

        match( Token_kind::Comma ); // optional
    }

    return true;
}

void Scanner::step_over_junk()
{
    if( check( Token_kind::L_brace ) )
    {
        skip_braces();
    }
    else
    {
        advance();
    }
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

    Head_scan own_head( u32 at = 0 )
    {
        return Scanner( tokens_ ).own_member_head( at, name_ );
    }

    Member_chunk chunk( u32 at = 0 )
    {
        return Scanner( tokens_ ).next_member( at, name_ );
    }

    Declaration_scan declaration( u32 at = 0 )
    {
        return Scanner( tokens_ ).declaration_head( at );
    }

    Declaration_chunk declaration_chunk( u32 at = 0 )
    {
        return Scanner( tokens_ ).next_declaration( at );
    }

    Scanner scanner() const
    {
        return Scanner( tokens_ );
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

void require_declaration( std::string_view source, Declaration_kind kind, std::string_view commit )
{
    Scanned          s( source );
    Declaration_scan scan = s.declaration();

    INFO( source );
    REQUIRE( scan.head.has_value() );
    REQUIRE( scan.head->kind == kind );
    REQUIRE( scan.head->start == 0 );
    REQUIRE( scan.head->commit == s.at( commit ) );
}

void require_declaration_failure( std::string_view source, std::string_view at, Wanted wanted )
{
    Scanned          s( source );
    Declaration_scan scan = s.declaration();

    INFO( source );
    REQUIRE_FALSE( scan.head.has_value() );
    REQUIRE( scan.failure.at == s.at( at ) );
    REQUIRE( scan.failure.wanted == wanted );
}

void require_declaration_failure( std::string_view source, std::string_view at, Token_kind token )
{
    Scanned          s( source );
    Declaration_scan scan = s.declaration();

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

// What ends a method body that was never closed: a constructor of some other name there is a
// function's head run into the body's last line.
TEST_CASE( "scanner_reads_a_constructor_by_the_class_name_only", "[scan]" )
{
    SECTION( "the class's own name" )
    {
        Scanned s( "C( i32 v ) { }" );
        REQUIRE( s.own_head().head.has_value() );
        REQUIRE( s.own_head().head->kind == Member_kind::Constructor );
    }

    SECTION( "another name" )
    {
        Scanned s( "D() { }" );
        REQUIRE( s.head().head.has_value() );
        REQUIRE_FALSE( s.own_head().head.has_value() );
    }

    SECTION( "the other kinds as member_head reads them" )
    {
        Scanned s( "public static i32 make() { }" );
        REQUIRE( s.own_head().head.has_value() );
        REQUIRE( s.own_head().head->kind == Member_kind::Method );
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

    // Or `a( 1 ) { }` heads a constructor named `a`, and its `1` is reported as a parameter.
    SECTION( "after a failed head, only the class's name opens a constructor" )
    {
        for( const std::string_view source : { "C() : a( 1 ) { } i32 b; }", "C() : a( 1 ), b( 2 ) { a = 3; } i32 b; }" } )
        {
            Scanned            s( source );
            const Member_chunk c = s.chunk();

            INFO( source );
            REQUIRE( c.dropped_begin == 0 );
            REQUIRE( c.dropped_end == s.at( "i32" ) );
            REQUIRE( c.failure.at == s.at( ":" ) );
            REQUIRE( c.failure.token == Token_kind::L_brace );
            REQUIRE( c.head.has_value() );
            REQUIRE( c.head->kind == Member_kind::Field );
        }
    }

    // A misnamed constructor at a member's start is still one, for sema to name.
    SECTION( "at a member's start, any name before `(` opens a constructor" )
    {
        Scanned            s( "a( i32 v ) { } }" );
        const Member_chunk c = s.chunk();

        REQUIRE_FALSE( c.dropped() );
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

TEST_CASE( "scanner_answers_the_parsers_lookahead", "[scan]" )
{
    auto declaration    = []( std::string_view source ) { return Scanned( source ).scanner().looks_like_declaration( 0 ); };
    auto binding        = []( std::string_view source ) { return Scanned( source ).scanner().looks_like_binding( 0 ); };
    auto type_arguments = []( std::string_view source )
    {
        const Scanned s( source );
        return s.scanner().looks_like_type_arguments( s.at( "<" ) );
    };

    SECTION( "a declaration is a type and then a name" )
    {
        CHECK( declaration( "i32 x = 0;" ) );
        CHECK( declaration( "const i32 x = 0;" ) );
        CHECK( declaration( "kl::Point p;" ) );
        CHECK( declaration( "Point* p;" ) );
        CHECK( declaration( "Vector<Vector<i32>> v;" ) );
        CHECK( declaration( "i32[*] xs;" ) );
        CHECK( declaration( "fn( i32 ) -> i32 f;" ) );
        CHECK( declaration( "field( Point ) -> i32 f;" ) );
    }

    SECTION( "a function type missing its arrow is still a declaration" )
    {
        CHECK( declaration( "fn( i32 ) i32 f;" ) );
    }

    SECTION( "a keyword, a digit-led name or a number holds the name's place" )
    {
        CHECK( declaration( "i32 out = 1;" ) );
        CHECK( declaration( "i32 3x;" ) );
        CHECK( declaration( "i32 3;" ) );
    }

    SECTION( "an expression is not a declaration" )
    {
        CHECK_FALSE( declaration( "f( 1 );" ) );
        CHECK_FALSE( declaration( "x = 1;" ) );
        CHECK_FALSE( declaration( "a < b;" ) );
        CHECK_FALSE( declaration( "return 1;" ) );
        CHECK_FALSE( declaration( "fn i32 f;" ) );
    }

    SECTION( "only a `*` or `&` touching the type is part of it" )
    {
        CHECK_FALSE( declaration( "a * b;" ) );
        CHECK_FALSE( declaration( "a & b;" ) );
        CHECK_FALSE( declaration( "Point *p;" ) );
    }

    SECTION( "a binding is a mode, then a declaration" )
    {
        CHECK( binding( "ref T r = x;" ) );
        CHECK( binding( "const ref T r = x;" ) );
        CHECK( binding( "move Box<i32> b = x;" ) );
        CHECK( binding( "out i32 o;" ) );

        CHECK_FALSE( binding( "const i32 x = 0;" ) );
        CHECK_FALSE( binding( "i32 x;" ) );
        CHECK_FALSE( binding( "ref x;" ) );
        CHECK_FALSE( declaration( "ref T r = x;" ) );
    }

    SECTION( "type arguments close before something no operand starts with" )
    {
        CHECK( type_arguments( "id<i32>( 1 )" ) );
        CHECK( type_arguments( "make<Box<i32>>()" ) );
        CHECK( type_arguments( "kl::id<kl::Point*>( p )" ) );
        CHECK( type_arguments( "id<i32>::make()" ) );
        CHECK( type_arguments( "a < b > ( c )" ) );
    }

    SECTION( "a comparison is not type arguments" )
    {
        CHECK_FALSE( type_arguments( "a < b > c" ) );
        CHECK_FALSE( type_arguments( "a < b;" ) );
        CHECK_FALSE( type_arguments( "a < 1 > ( c )" ) );
        CHECK_FALSE( type_arguments( "a < b >> c" ) );
    }

    SECTION( "each question starts where it is told" )
    {
        const Scanned s( "return 0; ref T r = x; i32 f() { } y = id<i32>( 1 );" );
        Scanner       scanner = s.scanner();

        CHECK( scanner.looks_like_binding( s.at( "ref" ) ) );
        CHECK( scanner.looks_like_declaration( s.at( "T" ) ) );
        CHECK( scanner.looks_like_type_arguments( s.at( "<" ) ) );
        CHECK_FALSE( scanner.looks_like_declaration( s.at( "return" ) ) );
    }
}

// A statement keyword where a body's name goes starts its statement once what it needs follows.
TEST_CASE( "scanner_ends_a_statement_head_at_a_statement_keyword", "[scan]" )
{
    auto declaration = []( std::string_view source ) { return Scanned( source ).scanner().looks_like_declaration( 0 ); };
    auto binding     = []( std::string_view source ) { return Scanned( source ).scanner().looks_like_binding( 0 ); };

    SECTION( "a keyword followed by what its statement needs" )
    {
        CHECK_FALSE( declaration( "u8 if ( c ) { }" ) );
        CHECK_FALSE( declaration( "i32 while ( n > 0 ) { }" ) );
        CHECK_FALSE( declaration( "i32 for ( ; ; ) { }" ) );
        CHECK_FALSE( declaration( "Colour switch ( c ) { }" ) );
        CHECK_FALSE( declaration( "u8 unsafe { }" ) );
        CHECK_FALSE( declaration( "Box return 2;" ) );
        CHECK_FALSE( binding( "ref i32 if ( c ) { }" ) );
    }

    SECTION( "otherwise the keyword is the misspelt name" )
    {
        CHECK( declaration( "i32 while x = 1;" ) );
        CHECK( declaration( "i32 if = 1;" ) );
        CHECK( declaration( "i32 return = 0;" ) );
        CHECK( declaration( "i32 return;" ) );
        CHECK( declaration( "i32 unsafe = 1;" ) );
        CHECK( declaration( "i32 break;" ) );
    }

    SECTION( "at file scope a keyword still names a function" )
    {
        const Declaration_scan scan = Scanned( "bool if( i32 n ) { }" ).declaration();

        REQUIRE( scan.head.has_value() );
        CHECK( scan.head->kind == Declaration_kind::Function );
    }
}

TEST_CASE( "scanner_recognises_each_declaration_head", "[scan]" )
{
    // Their parsers own everything after the keyword.
    SECTION( "a keyword-led declaration is committed at its keyword" )
    {
        require_declaration( "import a;", Declaration_kind::Import, "import" );
        require_declaration( "struct S { i32 a; };", Declaration_kind::Aggregate, "struct" );
        require_declaration( "class C<T> where T : Copyable { T a; };", Declaration_kind::Aggregate, "class" );
        require_declaration( "enum E { A, B };", Declaration_kind::Enum, "enum" );
        require_declaration( "struct S;", Declaration_kind::Aggregate, "struct" );
    }

    SECTION( "a function runs to its body's `{`" )
    {
        require_declaration( "i32 f() { return 1; }", Declaration_kind::Function, "{" );
        require_declaration( "i32 f( i32 a, ref i32 b ) { }", Declaration_kind::Function, "{" );
        require_declaration( "Vector<Box<i32>> make() { }", Declaration_kind::Function, "{" );
        require_declaration( "const ref T get( ref T a ) { }", Declaration_kind::Function, "{" );
        require_declaration( "fn( i32 ) -> i32 pick() { }", Declaration_kind::Function, "{" );
    }

    SECTION( "through type parameters and `where` clauses" )
    {
        require_declaration( "T id<T>( T a ) { }", Declaration_kind::Function, "{" );
        require_declaration( "T f<T, U>( T a, U b ) { }", Declaration_kind::Function, "{" );
        require_declaration( "T f<T>( T a ) where T : Copyable { }", Declaration_kind::Function, "{" );
        require_declaration( "T f<T>( T a ) where T : Copyable & Equatable { }", Declaration_kind::Function, "{" );
        require_declaration( "T f<T, U>( T a ) where T : Copyable, where U : Equatable { }", Declaration_kind::Function, "{" );
    }

    SECTION( "including what the parser reads only to refuse" )
    {
        require_declaration( "T f<>() { }", Declaration_kind::Function, "{" );
        require_declaration( "T f<Comparable T>( T a ) { }", Declaration_kind::Function, "{" );
        require_declaration( "T f<T>( T a ) where T : Copyable, Equatable { }", Declaration_kind::Function, "{" );
        require_declaration( "T f<T>( T a ) where T : Copyable | Equatable { }", Declaration_kind::Function, "{" );
    }

    SECTION( "an extern runs to its `;`, or to a body it should not have" )
    {
        require_declaration( "extern i32 abs( i32 v );", Declaration_kind::Function, ";" );
        require_declaration( "extern ref i32 at( ref i32 p );", Declaration_kind::Function, ";" );
        require_declaration( "extern i32 f() { return 1; }", Declaration_kind::Function, "{" );
    }

    SECTION( "a variable runs to its `=` or `;`" )
    {
        require_declaration( "i32 x = 1;", Declaration_kind::Variable, "=" );
        require_declaration( "i32 x;", Declaration_kind::Variable, ";" );
        require_declaration( "const i32 x = 1;", Declaration_kind::Variable, "=" );
        require_declaration( "kl::Point p;", Declaration_kind::Variable, ";" );
        require_declaration( "fn( i32 ) -> i32 f = g;", Declaration_kind::Variable, "=" );
    }

    SECTION( "a keyword, a digit-led name or a number holds the name's place" )
    {
        require_declaration( "i32 if;", Declaration_kind::Variable, ";" );
        require_declaration( "i32 3x = 1;", Declaration_kind::Variable, "=" );
        require_declaration( "i32 while() { }", Declaration_kind::Function, "{" );
    }
}

TEST_CASE( "scanner_says_where_and_why_a_declaration_head_fails", "[scan]" )
{
    SECTION( "nothing that can start a declaration" )
    {
        require_declaration_failure( "42;", "42", Wanted::Declaration );
        require_declaration_failure( "static i32 f() { }", "static", Wanted::Declaration );
        require_declaration_failure( "return 1;", "return", Wanted::Declaration );
        require_declaration_failure( "}", "}", Wanted::Declaration );
    }

    SECTION( "a body where `{` should be" )
    {
        require_declaration_failure( "i32 f();", ";", Token_kind::L_brace );
        require_declaration_failure( "i32 f() const { }", "const", Token_kind::L_brace );
        require_declaration_failure( "i32 f() -> i32 { }", "->", Token_kind::L_brace );
    }

    SECTION( "a variable that goes on past its name" )
    {
        require_declaration_failure( "i32 a, b;", ",", Token_kind::Semicolon );
        require_declaration_failure( "i32 a[4];", "[", Token_kind::Semicolon );
    }

    SECTION( "an extern is a function" )
    {
        require_declaration_failure( "extern i32 x;", ";", Token_kind::L_paren );
    }

    SECTION( "a missing name" )
    {
        require_declaration_failure( "i32 = 1;", "=", Wanted::Name );
    }

    SECTION( "type parameters left open" )
    {
        require_declaration_failure( "T f<T( T a ) { }", "(", Token_kind::Greater );
    }

    // The parser ends a clause at a bound that is not a name, short of the `{`.
    SECTION( "a `where` clause without its colon or a bound" )
    {
        require_declaration_failure( "T f<T>( T a ) where T { }", "{", Token_kind::Colon );
        require_declaration_failure( "T f<T>( T a ) where T : { }", "{", Wanted::Name );
        require_declaration_failure( "T f<T>( T a ) where T : if & A { }", "if", Wanted::Name );
    }
}

TEST_CASE( "scanner_chunks_a_file", "[scan]" )
{
    SECTION( "a declaration with nothing before it" )
    {
        Scanned                 s( "i32 a; i32 f() { }" );
        const Declaration_chunk c = s.declaration_chunk();

        REQUIRE_FALSE( c.dropped() );
        REQUIRE( c.head.has_value() );
        REQUIRE( c.head->kind == Declaration_kind::Variable );
        REQUIRE( c.head->start == 0 );
    }

    SECTION( "the end of the file" )
    {
        Scanned                 s( "" );
        const Declaration_chunk c = s.declaration_chunk();

        REQUIRE_FALSE( c.dropped() );
        REQUIRE_FALSE( c.head.has_value() );
    }

    // There is no enclosing body for it to close.
    SECTION( "a stray `}` is junk, not an end" )
    {
        Scanned                 s( "} i32 a;" );
        const Declaration_chunk c = s.declaration_chunk();

        REQUIRE( c.dropped_begin == 0 );
        REQUIRE( c.dropped_end == s.at( "i32" ) );
        REQUIRE( c.failure.wanted == Wanted::Declaration );
        REQUIRE( c.head->start == s.at( "i32" ) );
    }

    SECTION( "junk, then the declaration after it" )
    {
        Scanned                 s( "42; i32 a;" );
        const Declaration_chunk c = s.declaration_chunk();

        REQUIRE( c.dropped_end == s.at( "i32" ) );
        REQUIRE( c.failure.at == 0 );
        REQUIRE( c.failure.wanted == Wanted::Declaration );
        REQUIRE( c.head->kind == Declaration_kind::Variable );
    }

    SECTION( "a word before a function drops only the word" )
    {
        Scanned                 s( "static i32 f() { return 1; }" );
        const Declaration_chunk c = s.declaration_chunk();

        REQUIRE( c.dropped_end == s.at( "i32" ) );
        REQUIRE( c.head->kind == Declaration_kind::Function );
        REQUIRE( c.head->commit == s.at( "{" ) );
    }

    SECTION( "a keyword-led declaration after junk" )
    {
        Scanned                 s( "42 struct S { i32 a; };" );
        const Declaration_chunk c = s.declaration_chunk();

        REQUIRE( c.dropped_end == s.at( "struct" ) );
        REQUIRE( c.head->kind == Declaration_kind::Aggregate );
    }

    // Its body's statements are not declarations to resume at.
    SECTION( "a function whose head fails is dropped with its body" )
    {
        Scanned                 s( "i32 f() const { i32 inner; } i32 b;" );
        const Declaration_chunk c = s.declaration_chunk();

        REQUIRE( c.failure.at == s.at( "const" ) );
        REQUIRE( c.failure.token == Token_kind::L_brace );
        REQUIRE( c.dropped_end == s.at( "i32", 2 ) );
        REQUIRE( c.head->start == s.at( "i32", 2 ) );
    }

    SECTION( "a forward declaration is dropped through its `;`" )
    {
        Scanned                 s( "i32 f();\ni32 g() { }" );
        const Declaration_chunk c = s.declaration_chunk();

        REQUIRE( c.failure.at == s.at( ";" ) );
        REQUIRE( c.failure.token == Token_kind::L_brace );
        REQUIRE( c.dropped_end == s.at( "i32", 1 ) );
        REQUIRE( c.head->kind == Declaration_kind::Function );
    }

    SECTION( "a variable missing its `;` before another declaration is kept" )
    {
        Scanned                 s( "i32 a\ni32 f() { }" );
        const Declaration_chunk c = s.declaration_chunk();

        REQUIRE_FALSE( c.dropped() );
        REQUIRE( c.head->kind == Declaration_kind::Variable );
        REQUIRE( c.head->start == 0 );
        REQUIRE( c.head->commit == s.at( "i32", 1 ) );
    }

    SECTION( "an extern missing its `;` is kept as the function it is" )
    {
        Scanned                 s( "extern i32 abs( i32 v )\ni32 f() { }" );
        const Declaration_chunk c = s.declaration_chunk();

        REQUIRE_FALSE( c.dropped() );
        REQUIRE( c.head->kind == Declaration_kind::Function );
        REQUIRE( c.head->commit == s.at( "i32", 2 ) );
    }

    SECTION( "junk running to the end of the file" )
    {
        Scanned                 s( "i32 a; 1 2 3" );
        const Declaration_chunk c = s.declaration_chunk( s.at( "1" ) );

        REQUIRE( c.dropped_begin == s.at( "1" ) );
        REQUIRE( c.dropped_end == s.at( "3" ) + 1 );
        REQUIRE_FALSE( c.head.has_value() );
    }

    SECTION( "scanning starts where it is told" )
    {
        Scanned                 s( "i32 a; 42; i32 b;" );
        const Declaration_chunk c = s.declaration_chunk( s.at( "42" ) );

        REQUIRE( c.dropped_begin == s.at( "42" ) );
        REQUIRE( c.head->start == s.at( "i32", 1 ) );
    }
}

} // namespace keel

#endif // ENABLE_UNIT_TESTS
