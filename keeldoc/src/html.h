// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

namespace keeldoc
{

// `text` safe inside an element or a quoted attribute.
inline std::string html_escape( std::string_view text )
{
    std::string escaped;
    escaped.reserve( text.size() );

    for( const char c : text )
    {
        switch( c )
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped.push_back( c );
        }
    }

    return escaped;
}

} // namespace keeldoc
