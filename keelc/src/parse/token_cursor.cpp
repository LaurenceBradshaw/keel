// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parse/token_cursor.h"
#include <algorithm>
#include <cassert>

namespace keel
{

Token_cursor::Token_cursor( std::span<const Token> tokens )
    : tokens_( tokens )
{
}

const Token& Token_cursor::peek( u32 ahead ) const
{
    assert( !tokens_.empty() );
    return tokens_[std::min( at_ + ahead, narrow_cast<u32>( tokens_.size() - 1 ) )];
}

const Token& Token_cursor::previous() const
{
    assert( at_ > 0 && "previous() called at start of token stream" );
    return tokens_[at_ - 1];
}

bool Token_cursor::at_end() const
{
    return peek().kind == Token_kind::End_of_file;
}

bool Token_cursor::check( Token_kind kind ) const
{
    return peek().kind == kind;
}

bool Token_cursor::check_keyword( Keyword keyword ) const
{
    return peek().kind == Token_kind::Keyword && peek().keyword() == keyword;
}

bool Token_cursor::at_mode_keyword() const
{
    return check_keyword( Keyword::Move ) || check_keyword( Keyword::Ref ) || check_keyword( Keyword::Out );
}

bool Token_cursor::peek_is_adjacent() const
{
    return at_ > 0 && peek().span.file == previous().span.file && peek().span.start == previous().span.end;
}

const Token& Token_cursor::advance()
{
    if( at_end() )
    {
        return peek();
    }
    return tokens_[at_++];
}

bool Token_cursor::match( Token_kind kind )
{
    if( check( kind ) )
    {
        advance();
        return true;
    }
    return false;
}

bool Token_cursor::match_keyword( Keyword keyword )
{
    if( check_keyword( keyword ) )
    {
        advance();
        return true;
    }
    return false;
}

bool Token_cursor::match_generic_close()
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

        // Incremented rather than set: one owed close is all the grammar can produce, and a counter
        // that cannot lose one is worth more than the assumption.
        owed_greater_ += 1;
        return true;
    }

    return false;
}

bool Token_cursor::owes_greater() const
{
    return owed_greater_ > 0;
}

u32 Token_cursor::position() const
{
    return at_;
}

void Token_cursor::seek( u32 at )
{
    at_           = at;
    owed_greater_ = 0;
}

std::span<const Token> Token_cursor::tokens() const
{
    return tokens_;
}

} // namespace keel
