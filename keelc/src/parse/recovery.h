// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <vector>
#include "ast/ast.h"
#include "common/diagnostics.h"
#include "common/source_manager.h"
#include "parse/hints.h"
#include "parse/lookahead.h"
#include "parse/token_cursor.h"

namespace keel
{

// The parser's syntax errors and the recovery after them, so one mistake reports once and the
// parse goes on. Reads the parser's cursor and moves it; builds no node.
class Recovery
{
public:
    Recovery(
        Token_cursor&                 cursor,
        Lookahead&                    lookahead,
        const std::vector<Symbol_id>& classes,
        const Source_manager&         sm,
        Ast&                          ast,
        Diagnostics&                  diags
    );

    // --- reporting ---

    void error_at( Span span, std::string message, std::string help = {} );
    void fail_at( Span span, std::string message, std::string help = {} );

    // "expected `;`, found `,`".
    void error_expected( Token_kind kind, std::string help = {} );

    // Consume or report.
    bool expect( Token_kind kind );

    // What peek() should be called in a message: its source text where it has one, so "found
    // `widget`" rather than "found `identifier`".
    std::string found_text() const;
    std::string found_text( const Token& token ) const;

    u32  line_of( const Token& token ) const;
    bool gap_before( u32 at ) const;
    bool failed_since( u32 token ) const;

    // One error for the tokens a chunk drops, at the first word when the hint table knows it.
    void report_dropped( u32 begin, u32 end, const Scan_failure& failure, std::optional<u32> next, Hint_place place );

    // --- recovery ---

    // Panic mode: skip to something that plausibly starts a new statement.
    void synchronise();
    bool body_ends_here();
    void close_header( u32 condition_start );

    // A body's parse starts uncut; it ends at its `}` unless recovery already cut it short.
    void begin_body();
    void close_body();

    // A block opened without its `{`, so one more `}` will be unmatched.
    void open_without_brace();

    // The statement being parsed starts at `start`; returns the enclosing one's, to restore.
    std::optional<u32> begin_statement( u32 start );
    void               end_statement( std::optional<u32> enclosing );

    // --- lists ---

    enum class List_site : u8
    {
        Expression,
        Declaration,
        Literal
    };
    // After a list element: at `,` or `close` true; otherwise reports once per list and skips to
    // one, false when it stops short. `list_errors` is the error count before the list.
    bool end_list_element( Token_kind close, std::size_t list_errors, List_site site = List_site::Expression );

    // One closing `>`. On a miss, reports once and skips to this list's `>`; false means the list was broken.
    bool expect_generic_close();

private:
    bool at_statement_keyword() const;
    bool at_unclosed_body_head();
    void skip_past_closing_paren( u32 depth );

    // Past this list's `>`, or up to a token the list cannot reach past (`;`, `{`, an unmatched `)`).
    void skip_to_generic_close();

    Token_cursor&                 cursor_;
    Lookahead&                    lookahead_;
    const std::vector<Symbol_id>& classes_;
    const Source_manager&         sm_;
    Ast&                          ast_;
    Diagnostics&                  diags_;

    i32                unclosed_braces_ = 0;
    bool               body_cut_        = false;
    std::optional<u32> unclosed_at_;
    std::optional<u32> statement_start_;
    std::optional<u32> skipped_from_; // where the last list skip began
};

} // namespace keel
