#pragma once

#include <string>
#include <string_view>

namespace keel
{

// Escapes the quotes and backslashes a string or char literal contains, so a dump line stays
// unambiguously parseable when the golden runner diffs it. Shared by --dump-tokens and --dump-ast.
inline std::string escape_for_dump( std::string_view text )
{
    std::string out;
    out.reserve( text.size() );

    for( const char c : text )
    {
        if( c == '"' || c == '\\' )
        {
            out.push_back( '\\' );
        }
        out.push_back( c );
    }

    return out;
}

} // namespace keel
