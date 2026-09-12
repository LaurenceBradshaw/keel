#pragma once
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
    std::string help = {}; // Empty when no help is available.
};

class Diagnostics
{
public:
    void error( Span span, std::string message, std::string help = {} );
    void warning( Span span, std::string message, std::string help = {} );

    bool   has_errors() const;
    size_t error_count() const;

    // colour is the caller's decision: render() writes to any ostream, and only the driver knows
    // whether its destination is a terminal. See colour_supported().
    void render( const Source_manager& sm, std::ostream& out, bool colour = false ) const;

private:
    std::vector<Diagnostic> items_;
};

// True when stderr is a terminal and NO_COLOR is unset (https://no-color.org). The golden runner
// redirects stderr to a file, so its output stays uncoloured with no special handling.
bool colour_supported();

} // namespace keel