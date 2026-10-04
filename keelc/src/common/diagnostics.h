// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <map>
#include <string>
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

struct Diagnostic
{
    Severity    severity;
    Span        span;
    std::string message;
    std::string help   = {}; // Empty when no help is available.
    bool        syntax = false;
};

class Diagnostics
{
public:
    void error( Span span, std::string message, std::string help = {} );
    void syntax_error( Span span, std::string message, std::string help = {} );
    void warning( Span span, std::string message, std::string help = {} );

    void tokens( File_id file, std::vector<Span> bounds );

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

    std::vector<Diagnostic>          items_;
    std::map<u32, std::vector<Span>> tokens_; // keyed by File_id::v
};

// True when stderr is a terminal and NO_COLOR is unset (https://no-color.org). The golden runner
// redirects stderr to a file, so its output stays uncoloured with no special handling.
bool colour_supported();

} // namespace keel