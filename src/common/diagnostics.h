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

    void render( const Source_manager& sm, std::ostream& out ) const;

private:
    std::vector<Diagnostic> items_;
};

} // namespace keel