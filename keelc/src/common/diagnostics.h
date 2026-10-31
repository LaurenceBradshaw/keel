// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "common/source_manager.h"
#include "common/span.h"

namespace keel
{

enum class Severity
{
    Error,
    Warning,
    Note
};

enum class Warning : u8
{
    Unused_parameter,
    Unused_variable,
    Void_variable,
    Constant_comparison,
    Needless_move,
    Unreachable_code,
    Large_local // off unless -Wlarge-local
};

std::string_view warning_name( Warning which );
// Empty for a name no warning has.
std::optional<Warning> warning_named( std::string_view name );

struct Diagnostic
{
    Severity               severity;
    Span                   span;
    std::string            message;
    std::string            help    = {}; // Empty when no help is available.
    bool                   syntax  = false;
    std::optional<Warning> warning = {}; // Set on a warning, and on an error that -Werror made of one
};

// One mistake gets one error. Errors that only follow from another are dropped when rendered or
// counted, by three rules applied in order:
//
//   silenced    a non-syntax error starting in a silence()d span
//   claimed     an error overlapping a token an earlier-emitted syntax error covers
//   unbalanced  in a file with an unbalanced() brace, every error but that and the earliest other
//
// Warnings, notes and errors without a span are always kept.
class Diagnostics
{
public:
    void error( Span span, std::string message, std::string help = {} );
    // Claims the tokens it covers, if the file's bounds are set.
    void syntax_error( Span span, std::string message, std::string help = {} );
    // A brace the lexer could not pair: its file keeps this and one other error.
    void unbalanced( Span span, std::string message, std::string help = {} );
    void warning( Warning which, Span span, std::string message, std::string help = {} );

    // flag is what follows -W, read left to right as the command line gives them; false for a name
    // no warning has.
    bool apply_warning_flag( std::string_view flag );

    // The file's token spans, in order, so a syntax error can claim them.
    void set_token_bounds( File_id file, std::vector<Span> bounds );
    // A construct the parser misread: the checker's judgments starting in it are dropped.
    void silence( Span span );

    bool   has_errors() const;
    size_t error_count() const;

    // colour is the caller's decision: render() writes to any ostream, and only the driver knows
    // whether its destination is a terminal. See colour_supported().
    void render( const Source_manager& sm, std::ostream& out, bool colour = false ) const;

    // JSON Lines for an editor: one {"kind":"file"} per loaded file, then one {"kind":"diagnostic"}
    // each. Lines and columns are 1-based bytes; end is exclusive.
    void render_json( const Source_manager& sm, std::ostream& out ) const;

private:
    std::vector<u32>  in_source_order() const;
    std::vector<bool> kept() const;
    void              drop_silenced( std::vector<bool>& keep ) const;
    void              drop_claimed( std::vector<bool>& keep ) const;
    void              keep_first_after_unbalanced( std::vector<bool>& keep ) const;

    enum class Level : u8
    {
        On,
        Off,
        Error
    };

    std::vector<Diagnostic>          items_;
    std::map<u32, std::vector<Span>> tokens_; // keyed by File_id::v
    std::vector<Span>                silenced_;
    std::map<u32, u32>               unbalanced_; // mapping a file to the index of its brace report in items_
    // Indexed by Warning: every one on but large-local.
    std::array<Level, 7> levels_ { Level::On, Level::On, Level::On, Level::On, Level::On, Level::On, Level::Off };
    bool                 all_errors_ = false; // a bare -Werror
};

// True when stderr is a terminal and NO_COLOR is unset (https://no-color.org). The golden runner
// redirects stderr to a file, so its output stays uncoloured with no special handling.
bool colour_supported();

} // namespace keel