// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include "common/types.h"
#include "lex/token.h"

namespace keel
{

// A position in a token stream, shared by the parser and its lookahead. peek() clamps to the
// End_of_file token, so no rule needs a bounds check.
class Token_cursor
{
public:
    explicit Token_cursor( std::span<const Token> tokens );

    const Token& peek( u32 ahead = 0 ) const;

    // The last consumed token. Every rule's span is merge( first token, previous() ).
    const Token& previous() const;

    bool at_end() const;
    bool check( Token_kind kind ) const;
    bool check_keyword( Keyword keyword ) const;
    bool at_mode_keyword() const;

    // Whether peek() begins exactly where the previous token ended. PLAN §6.3 D17 uses it to bind
    // `*` to the type: `u32* p` is a pointer declaration, `u32 * p` is a multiplication.
    bool peek_is_adjacent() const;

    const Token& advance();
    bool         match( Token_kind kind );
    bool         match_keyword( Keyword keyword );

    // One closing `>`. `Vector<Vector<i32>>` closes with a single `>>` token, so the second half is
    // owed rather than the token stream mutated.
    bool match_generic_close();

    // Half of a `>>` is still owed to an enclosing list, and every token after it is that list's.
    bool owes_greater() const;

    u32 position() const;

    // Moves to `at`, dropping any owed `>`: every jump starts a fresh rule.
    void seek( u32 at );

    std::span<const Token> tokens() const;

private:
    std::span<const Token> tokens_;
    u32                    at_           = 0;
    u32                    owed_greater_ = 0;
};

} // namespace keel
