#pragma once

#include "common/types.h"

#include <algorithm>
#include <cassert>
#include <type_traits>

namespace keel
{

// Not 0: file 0 is valid, and Span { file 0, 0, 0 } is a real zero-length span.
inline constexpr u32 k_invalid_file = 0xFFFF'FFFFu;

// A handle into Source_manager's file table. Lives here, not in source_manager.h, because Span
// stores one by value while source_manager.h already depends on Span.
struct File_id
{
    u32 v = k_invalid_file;

    bool is_valid() const
    {
        return v != k_invalid_file;
    }

    auto operator<=>( const File_id& other ) const = default;
};

// A half-open byte range [start, end) in one source file, owned by nothing and passed by value.
// Every AST node, type, and IR instruction carries one.
//
// Byte offsets rather than line/column: line/column is a presentation format, and the other
// consumers (text extraction, #line emission) want offsets. Source_manager converts on demand.
//
// [start, end) rather than { start, len } because merge() runs on every node the parser builds, and
// min/max of two pairs is harder to get wrong than length arithmetic.
struct Span
{
    File_id file;
    u32     start = 0;
    u32     end   = 0;

    u32 len() const
    {
        return end - start;
    }

    // Zero-length spans are legitimate: EOF tokens, and "expected ';'" pointing between two tokens.
    // Renderers draw a bare caret for these.
    bool is_empty() const
    {
        return start == end;
    }

    bool is_valid() const
    {
        return file.is_valid();
    }

    bool contains( Span other ) const
    {
        return file == other.file && other.start >= start && other.end <= end;
    }

    // Smallest span covering both. Absorbs an invalid operand, because the parser merges against
    // nodes that failed to parse.
    static Span merge( Span a, Span b )
    {
        if( !a.is_valid() )
        {
            return b;
        }
        if( !b.is_valid() )
        {
            return a;
        }

        // A diagnostic covering two files wants two labels, not one span.
        assert( a.file == b.file && "cannot merge spans from different files" );

        return Span { a.file, std::min( a.start, b.start ), std::max( a.end, b.end ) };
    }

    // A zero-length position, for EOF tokens and "expected X here".
    static Span point( File_id file, u32 offset )
    {
        return Span { file, offset, offset };
    }

    // Rarely correct. A generated node should inherit the span of whatever caused it - an inserted
    // destructor call gets the closing brace that triggered it. Every none() is a diagnostic that
    // cannot point anywhere.
    static Span none()
    {
        return Span {};
    }

    auto operator<=>( const Span& other ) const = default;
};

// Span sits in every node in the compiler, so its size is load-bearing.
static_assert( sizeof( Span ) == 12, "Span must stay 12 bytes" );
static_assert( std::is_trivially_copyable_v<Span>, "Span must stay trivially copyable" );

} // namespace keel
