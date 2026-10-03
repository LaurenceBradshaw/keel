// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <span>
#include "common/interner.h"
#include "common/types.h"
#include "lex/token.h"

namespace keel
{

enum class Member_kind : u8
{
    Field,
    Static_var,
    Method,
    Constructor,
    Destructor,
};

// What a scan wanted where it stopped.
enum class Wanted : u8
{
    Token, // the kind in Scan_failure::token
    Member,
    Type,
    Name,
};

struct Scan_failure
{
    u32        at     = 0; // index of the token the scan stopped on
    Wanted     wanted = Wanted::Member;
    Token_kind token  = Token_kind::End_of_file; // meaningful only when wanted is Token
};

// Which parse function a member needs, and where. Nothing is built.
struct Member_head
{
    Member_kind kind   = Member_kind::Field;
    u32         start  = 0; // first token, access marker and `static` included
    u32         commit = 0; // the body's `{`, a static's `=`, or where a field's `;` is or should be
};

struct Head_scan
{
    std::optional<Member_head> head;
    Scan_failure               failure; // meaningful only when head is empty
};

// One step of a class body: tokens to drop, then a member - or the body's end.
struct Member_chunk
{
    u32                        dropped_begin = 0;
    u32                        dropped_end   = 0; // equal to dropped_begin when nothing is dropped
    Scan_failure               failure;           // why dropped_begin could not start a member
    std::optional<Member_head> head;              // empty at the body's `}` or the end of the file

    bool dropped() const
    {
        return dropped_end != dropped_begin;
    }
};

// Lookahead for the parser. Reads tokens and reports nothing. Its cursor and its half of a split
// `>>` are its own, so no scan can leave either behind in the parser.
class Scanner
{
public:
    explicit Scanner( std::span<const Token> tokens );

    // The member head starting at `at`. `enclosing` is the class's name.
    Head_scan member_head( u32 at, Symbol_id enclosing );

    // Starting at `at`: the tokens no member can be read from, then the member after them.
    Member_chunk next_member( u32 at, Symbol_id enclosing );

private:
    // Whether any name before `(` opens a constructor, or only the class's own.
    enum class Constructor_names : u8
    {
        Any,
        Enclosing_only,
    };

    Head_scan scan_head( u32 at, Symbol_id enclosing, Constructor_names names );

    // --- cursor. peek() clamps to the End_of_file token. ---

    const Token& peek( u32 ahead = 0 ) const;
    bool         at_end() const;
    bool         check( Token_kind kind ) const;
    bool         check_keyword( Keyword keyword ) const;
    bool         at_mode_keyword() const;

    void advance();
    bool match( Token_kind kind );
    bool match_keyword( Keyword keyword );

    // --- pieces. Each consumes what it names and returns true, or records failure_ and returns false. ---

    // Returns false so a scan can `return fail( ... )`; called for its record alone elsewhere.
    bool fail( Wanted wanted, Token_kind token = Token_kind::End_of_file );
    bool want( Token_kind kind );
    bool want_name();

    // The type grammar parse_type_with_mode and parse_type accept, including what they consume
    // only to report, such as `T&`.
    bool scan_type_with_mode();
    bool scan_type();
    bool scan_generic_close();

    // At `(`: past its matching `)`.
    bool skip_parens();

    // At `{`: past its matching `}`, or to the end of the file.
    void skip_braces();

    std::span<const Token> tokens_;
    u32                    cursor_       = 0;
    u32                    owed_greater_ = 0;
    Scan_failure           failure_;
};

} // namespace keel
