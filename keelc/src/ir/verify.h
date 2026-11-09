// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <vector>
#include "ir/kir.h"

namespace keel
{

// Where a problem is, spelled only when there is one: `statement 4 argument 1`.
struct At
{
    std::string_view kind;
    u32              index    = 0;
    std::string_view part     = {};
    i64              argument = -1;

    At operator()( std::string_view in, i64 number = -1 ) const
    {
        return At { kind, index, in, number };
    }
};

inline std::string format_as( const At& at )
{
    std::string text = fmt::format( "{} {}", at.kind, at.index );
    if( !at.part.empty() )
    {
        text += fmt::format( " {}", at.part );
    }
    if( at.argument >= 0 )
    {
        text += fmt::format( " {}", at.argument );
    }
    return text;
}

std::vector<std::string> verify( const Function& func );

} // namespace keel