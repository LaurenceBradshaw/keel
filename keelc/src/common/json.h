// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <fmt/format.h>
#include <string>
#include <string_view>

namespace keel
{

// A JSON string body: quotes, backslashes and control characters escaped, everything else as is.
inline std::string json_escape( std::string_view text )
{
    std::string escaped;
    escaped.reserve( text.size() );

    for( const char c : text )
    {
        switch( c )
        {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if( static_cast<unsigned char>( c ) < 0x20 )
            {
                escaped += fmt::format( "\\u{:04x}", static_cast<unsigned>( c ) );
            }
            else
            {
                escaped.push_back( c );
            }
        }
    }

    return escaped;
}

} // namespace keel
