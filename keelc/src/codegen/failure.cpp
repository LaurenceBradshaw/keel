// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/failure.h"

namespace keel
{

std::string_view failure_prefix( Failure failure )
{
    switch( failure )
    {
    case Failure::Assert:
        return "assertion failed: ";
    case Failure::Message:
        return "panic: ";
    case Failure::Cast:
        return "cast out of range: ";
    case Failure::Division:
        return "division by zero: ";
    case Failure::Remainder:
        return "remainder by zero: ";
    case Failure::Unordered:
        return "NaN has no order: ";
    case Failure::Unhashable:
        return "NaN has no hash: ";
    case Failure::Bounds:
        return "out of bounds: ";
    case Failure::Overflow:
        return "arithmetic overflow: ";
    }
    return "";
}

std::string as_written( const Source_manager& sm, Span span )
{
    std::string text;

    for( const char c : sm.text( span ) )
    {
        if( c != ' ' && c != '\t' && c != '\n' && c != '\r' )
        {
            text.push_back( c );
        }
        else if( !text.empty() && text.back() != ' ' )
        {
            text.push_back( ' ' );
        }
    }

    return text;
}

} // namespace keel
