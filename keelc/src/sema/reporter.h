#pragma once
#include <string>
#include <string_view>
#include "common/diagnostics.h"
#include "common/source_manager.h"
#include "common/span.h"

namespace keel::sema
{

// Owns the two things reporting needs - the sink and the source text - so that no rule class has
// to hold either, and none of them has to name the checker in order to report.
//
// It takes spans, never nodes. A class that cannot reach the tree cannot decide, on its way to
// reporting, to check one more thing.
class Reporter
{
public:
    Reporter( const Source_manager& source_manager, Diagnostics& diags )
        : sm_( source_manager ),
          diags_( diags )
    {
    }

    void error_at( Span span, std::string message, std::string help = {} );
    void warn_at( Span span, std::string message, std::string help = {} );

    std::string previous_declaration_note( Span previous ) const;

    // For a help line that quotes the author back.
    std::string_view text( Span span ) const;

    // The speculative paths compare this before and after, and discard an attempt that reported.
    std::size_t error_count() const;

private:
    const Source_manager& sm_;
    Diagnostics&          diags_;
};

} // namespace keel::sema
