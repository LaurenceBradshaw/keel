// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parse/recovery.h"

#include <fmt/format.h>
#include <cassert>

namespace keel
{
namespace
{

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

} // namespace

Recovery::Recovery(
    Token_cursor&                 cursor,
    Lookahead&                    lookahead,
    const std::vector<Symbol_id>& classes,
    const Source_manager&         sm,
    Ast&                          ast,
    Diagnostics&                  diags
)
    : cursor_( cursor ),
      lookahead_( lookahead ),
      classes_( classes ),
      sm_( sm ),
      ast_( ast ),
      diags_( diags )
{
    u32 l_brace_count = 0;
    u32 r_brace_count = 0;
    for( const Token& token : cursor_.tokens() )
    {
        if( token.kind == Token_kind::L_brace )
        {
            l_brace_count++;
        }
        else if( token.kind == Token_kind::R_brace )
        {
            r_brace_count++;
        }
    }

    unclosed_braces_ = narrow_cast<i32>( l_brace_count ) - narrow_cast<i32>( r_brace_count );
}

bool Recovery::expect( Token_kind kind )
{
    if( !cursor_.match( kind ) )
    {
        error_expected( kind );
        return false;
    }

    return true;
}

void Recovery::error_at( Span span, std::string message, std::string help )
{
    diags_.syntax_error( span, std::move( message ), std::move( help ) );
}

void Recovery::fail_at( Span span, std::string message, std::string help )
{
    ast_.fail( span );
    error_at( span, std::move( message ), std::move( help ) );
}

void Recovery::error_expected( Token_kind kind, std::string help )
{
    // Point just past the last token we accepted, not at the one we found. A missing `;` is missing
    // at the end of the previous line, which is where the reader looks - pointing at the `}` on the
    // next line describes the symptom rather than the mistake.
    const Span at =
        cursor_.position() > 0 ? Span::point( cursor_.previous().span.file, cursor_.previous().span.end ) : cursor_.peek().span;

    // After a list skipped tokens, where the statement ends is a guess.
    if( kind == Token_kind::Semicolon && statement_start_ && skipped_from_ && *skipped_from_ >= *statement_start_ )
    {
        ast_.fail( at );
        return;
    }

    // A keyword where a name was wanted is worth saying out loud. `out`, `ref` and `move` are
    // ordinary identifiers in C++, so a program can arrive here without its author suspecting that
    // the name is the problem.
    if( help.empty() && kind == Token_kind::Identifier && cursor_.check( Token_kind::Keyword ) )
    {
        help = fmt::format( "{} is a keyword, so it cannot be used as a name", found_text() );
    }

    fail_at( at, fmt::format( "expected {}, found {}", expectation( kind ), found_text() ), std::move( help ) );
}

std::string Recovery::found_text() const
{
    return found_text( cursor_.peek() );
}

std::string Recovery::found_text( const Token& token ) const
{
    // Quoted, because it is text the author actually wrote - except at the end of the file, where
    // there is nothing to quote and "found `end of file`" reads as though they typed that.
    if( token.kind == Token_kind::End_of_file )
    {
        return std::string( token_kind_spelling( Token_kind::End_of_file ) );
    }

    return fmt::format( "`{}`", sm_.text( token.span ) );
}

void Recovery::synchronise()
{
    // Skips to the end of a statement, balancing `(` and `[`; a block stops it at any depth.

    u32 depth  = 0;
    u32 braces = 0;

    while( !cursor_.at_end() )
    {
        if( depth == 0 && cursor_.match( Token_kind::Semicolon ) )
        {
            return;
        }

        if( at_unclosed_body_head() )
        {
            return;
        }

        switch( cursor_.peek().kind )
        {
        case Token_kind::End_of_file:
            return;
        case Token_kind::R_brace:
            if( braces > 0 )
            {
                braces--;
                break;
            }
            else
            {
                return;
            }

        case Token_kind::Keyword:
            if( at_statement_keyword() )
            {
                return;
            }
            break;
        case Token_kind::L_paren:
        case Token_kind::L_bracket:
            ++depth;
            break;
        case Token_kind::R_paren:
        case Token_kind::R_bracket:
            if( depth > 0 )
            {
                --depth;
            }
            break;
        case Token_kind::L_brace:
        {
            const bool opens_literal = cursor_.previous().kind == Token_kind::Identifier ||
                                       cursor_.previous().kind == Token_kind::Greater ||
                                       cursor_.previous().kind == Token_kind::Greater_greater;
            if( !opens_literal )
            {
                return;
            }
            braces++;
            break;
        }

        default:
            break;
        }

        cursor_.advance();
    }
}

bool Recovery::at_statement_keyword() const
{
    if( cursor_.peek().kind != Token_kind::Keyword )
    {
        return false;
    }

    switch( cursor_.peek().keyword() )
    {
    case Keyword::Return:
    case Keyword::If:
    case Keyword::While:
    case Keyword::For:
    case Keyword::Switch:
    case Keyword::Break:
    case Keyword::Continue:
    case Keyword::Fallthrough:
    case Keyword::Unsafe:
        return true;

    default:
        return false;
    }
}

bool Recovery::at_unclosed_body_head()
{
    if( unclosed_braces_ <= 0 )
    {
        return false;
    }

    if( !classes_.empty() )
    {
        const Head_scan member = lookahead_.own_member_head( cursor_.position(), classes_.back() );
        if( member.head.has_value() && member.head->kind != Member_kind::Field )
        {
            return true;
        }
    }

    const Declaration_scan scan = lookahead_.declaration_head( cursor_.position(), Lookahead::Scan_site::Statement );
    return scan.head.has_value() && scan.head->kind != Declaration_kind::Variable;
}

bool Recovery::body_ends_here()
{
    if( body_cut_ )
    {
        return true;
    }

    if( !at_unclosed_body_head() )
    {
        return false;
    }

    error_expected( Token_kind::R_brace );
    unclosed_braces_--;
    body_cut_ = true;
    return true;
}

void Recovery::skip_past_closing_paren( u32 depth )
{
    while( !cursor_.at_end() && !cursor_.check( Token_kind::L_brace ) && !cursor_.check( Token_kind::R_brace ) &&
           !at_statement_keyword() )
    {
        if( cursor_.check( Token_kind::L_paren ) )
        {
            ++depth;
        }
        else if( cursor_.check( Token_kind::R_paren ) )
        {
            --depth;
            if( depth == 0 )
            {
                cursor_.advance();
                return;
            }
        }

        cursor_.advance();
    }
}

// Closes a control-flow header, skipping what is left of a condition that broke.
void Recovery::close_header( u32 condition_start )
{
    if( !cursor_.match( Token_kind::R_paren ) )
    {
        if( !failed_since( condition_start ) )
        {
            error_expected( Token_kind::R_paren );
        }
        u32 depth = 1;
        for( u32 i = condition_start; i < cursor_.position(); ++i )
        {
            const Token& token = cursor_.tokens()[i];

            if( token.kind == Token_kind::L_paren )
            {
                ++depth;
            }
            else if( token.kind == Token_kind::R_paren )
            {
                if( depth > 1 )
                {
                    --depth;
                }
            }
        }
        skip_past_closing_paren( depth );
    }
}

void Recovery::report_dropped( u32 begin, u32 end, const Scan_failure& failure, std::optional<u32> next, Hint_place place )
{
    assert( begin < end );

    const Token& first = cursor_.tokens()[begin];
    const Token& last  = cursor_.tokens()[end - 1];

    const bool same_line = next && line_of( last ) == line_of( cursor_.tokens()[*next] );

    const std::string_view word_hint = dropped_word_hint( sm_.text( first.span ), place );

    Span        span;
    std::string message;
    std::string help;
    if( same_line )
    {
        span    = Span::merge( first.span, last.span );
        message = fmt::format(
            "expected a {}, found `{}`", place == Hint_place::Member ? "member" : "declaration", sm_.text( span )
        );
        help = !word_hint.empty() ? std::string( word_hint )
               : failure.wanted == Wanted::Token && failure.at < end
                   ? std::string( stop_hint( failure.token, cursor_.tokens()[failure.at].kind, place ) )
                   : std::string {};
    }
    else if( !word_hint.empty() || failure.at >= end )
    {
        span    = first.span;
        message = fmt::format(
            "expected a {}, found `{}`", place == Hint_place::Member ? "member" : "declaration", sm_.text( span )
        );
        help = std::string( word_hint );
    }
    else
    {
        const Token& stop = cursor_.tokens()[failure.at];
        span              = stop.span;

        // A `{` opens its own line; anything else wanted is missing at the gap.
        if( gap_before( failure.at ) && !( failure.wanted == Wanted::Token && failure.token == Token_kind::L_brace ) )
        {
            span = Span::point( cursor_.tokens()[failure.at - 1].span.file, cursor_.tokens()[failure.at - 1].span.end );
        }

        message = "expected ";
        switch( failure.wanted )
        {
        case Wanted::Token:
            message += expectation( failure.token );
            help = std::string( stop_hint( failure.token, stop.kind, place ) );
            break;
        case Wanted::Member:
            message += "a member";
            break;
        case Wanted::Declaration:
            message += "a declaration";
            break;
        case Wanted::Type:
            message += "a type";
            break;
        case Wanted::Name:
            message += "an identifier";
            break;
        }
        message += fmt::format( ", found {}", found_text( stop ) );
    }

    fail_at( span, std::move( message ), std::move( help ) );
}

u32 Recovery::line_of( const Token& token ) const
{
    return sm_.line_col( token.span.file, token.span.start ).line;
}

// Whether the token at `at` starts a later line with no separator before it.
bool Recovery::gap_before( u32 at ) const
{
    if( at <= 0 || at >= cursor_.tokens().size() )
    {
        return false;
    }

    const Token&     at_token     = cursor_.tokens()[at];
    const Token&     before_token = cursor_.tokens()[at - 1];
    const Token_kind before_kind  = cursor_.tokens()[at - 1].kind;
    return before_token.span.file == at_token.span.file && line_of( before_token ) < line_of( at_token ) &&
           before_kind != Token_kind::Semicolon && before_kind != Token_kind::L_brace && before_kind != Token_kind::R_brace &&
           before_kind != Token_kind::Comma;
}

bool Recovery::expect_generic_close()
{
    if( cursor_.match_generic_close() )
    {
        return true;
    }

    if( !unclosed_at_ || cursor_.position() != *unclosed_at_ )
    {
        error_expected( Token_kind::Greater );
    }

    skip_to_generic_close();
    return false;
}

bool Recovery::end_list_element( Token_kind close, std::size_t list_errors, List_site site )
{
    if( cursor_.check( Token_kind::Comma ) || cursor_.check( close ) )
    {
        return true;
    }

    if( diags_.error_count() == list_errors )
    {
        const Span at = cursor_.position() > 0 ? Span::point( cursor_.previous().span.file, cursor_.previous().span.end )
                                               : cursor_.peek().span;
        fail_at( at, fmt::format( "expected `,` or {}, found {}", expectation( close ), found_text() ) );
    }

    const bool braces    = site != List_site::Expression;
    const u32  skip_from = cursor_.position();
    u32        depth     = 0;
    while( !cursor_.at_end() )
    {
        const bool opens = cursor_.check( Token_kind::L_paren ) || cursor_.check( Token_kind::L_bracket ) ||
                           ( braces && cursor_.check( Token_kind::L_brace ) );
        const bool closes = cursor_.check( Token_kind::R_paren ) || cursor_.check( Token_kind::R_bracket ) ||
                            ( braces && cursor_.check( Token_kind::R_brace ) );

        // A literal holds no statement, so a `;` at any depth ends it.
        if( site == List_site::Literal && cursor_.check( Token_kind::Semicolon ) )
        {
            break;
        }

        if( depth == 0 )
        {
            if( cursor_.check( Token_kind::Comma ) || cursor_.check( close ) )
            {
                break;
            }

            if( site == List_site::Declaration )
            {
                if( lookahead_.declaration_head( cursor_.position() ).head.has_value() )
                {
                    break;
                }

                // A name starting a line is the next variant, its `,` missing.
                if( cursor_.check( Token_kind::Identifier ) && line_of( cursor_.peek() ) != line_of( cursor_.previous() ) )
                {
                    return true;
                }
            }
            // Not at `{` or a statement keyword: they are the element's fault, not the list's end.
            else if( site == List_site::Literal )
            {
                if( closes )
                {
                    break;
                }
            }
            else if( closes || cursor_.check( Token_kind::Semicolon ) || cursor_.check( Token_kind::L_brace ) ||
                     cursor_.check( Token_kind::R_brace ) || at_statement_keyword() )
            {
                break;
            }
        }

        if( opens )
        {
            depth += 1;
        }
        else if( closes && depth > 0 )
        {
            depth -= 1;
        }

        cursor_.advance();
    }

    if( cursor_.check( Token_kind::Comma ) || cursor_.check( close ) )
    {
        if( cursor_.position() > skip_from )
        {
            skipped_from_ = skip_from;
        }
        return true;
    }

    // Short of both: the statement's own skip knows more.
    if( site != List_site::Declaration )
    {
        cursor_.seek( skip_from );
    }
    return false;
}

void Recovery::skip_to_generic_close()
{
    u32 depth_less          = 0;
    u32 depth_paren_bracket = 0;

    while( true )
    {
        if( cursor_.check( Token_kind::Semicolon ) || cursor_.check( Token_kind::L_brace ) ||
            cursor_.check( Token_kind::R_brace ) || cursor_.check( Token_kind::Equal ) || cursor_.at_end() ||
            ( ( cursor_.check( Token_kind::R_paren ) || cursor_.check( Token_kind::R_bracket ) ) && depth_paren_bracket == 0 ) )
        {
            unclosed_at_ = cursor_.position();
            break;
        }

        if( ( cursor_.check( Token_kind::Greater ) || cursor_.check( Token_kind::Greater_greater ) ) && depth_less == 0 )
        {
            cursor_.match_generic_close();
            break;
        }
        if( cursor_.check( Token_kind::Greater_greater ) && depth_less == 1 )
        {
            cursor_.advance();
            break;
        }

        if( cursor_.check( Token_kind::Less ) )
        {
            depth_less += 1;
        }
        else if( cursor_.check( Token_kind::Greater ) )
        {
            depth_less -= 1;
        }
        else if( cursor_.check( Token_kind::Greater_greater ) )
        {
            depth_less -= 2;
        }
        else if( cursor_.check( Token_kind::L_paren ) || cursor_.check( Token_kind::L_bracket ) )
        {
            depth_paren_bracket += 1;
        }
        else if( cursor_.check( Token_kind::R_paren ) || cursor_.check( Token_kind::R_bracket ) )
        {
            depth_paren_bracket -= 1;
        }

        cursor_.advance();
    }
}

bool Recovery::failed_since( u32 token ) const
{
    return cursor_.position() > token &&
           ast_.failed_within( Span::merge( cursor_.tokens()[token].span, cursor_.previous().span ) );
}

void Recovery::begin_body()
{
    body_cut_ = false;
}

void Recovery::close_body()
{
    if( !body_cut_ )
    {
        expect( Token_kind::R_brace );
    }
}

void Recovery::open_without_brace()
{
    unclosed_braces_++;
}

std::optional<u32> Recovery::begin_statement( u32 start )
{
    const std::optional<u32> enclosing = statement_start_;
    statement_start_                   = start;
    return enclosing;
}

void Recovery::end_statement( std::optional<u32> enclosing )
{
    statement_start_ = enclosing;
}

} // namespace keel
