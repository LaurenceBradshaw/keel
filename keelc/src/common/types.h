#pragma once

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace keel
{

// Use these where the width is load-bearing: handles, byte offsets, node fields, Keel literal
// values. Container sizes stay std::size_t and text characters stay char - narrow once, at the
// boundary where a value becomes a handle or an offset, via narrow_cast.

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

// The single greppable narrowing point: asserts in debug, plain static_cast in release.
template <typename To, typename From>
constexpr To narrow_cast( From value ) noexcept
{
    static_assert( std::is_arithmetic_v<To>, "narrow_cast target must be arithmetic" );
    static_assert( std::is_arithmetic_v<From>, "narrow_cast source must be arithmetic" );

    const To result = static_cast<To>( value );

    if constexpr( std::is_integral_v<To> && std::is_integral_v<From> )
    {
        // Not a round-trip check: narrow_cast<u32>( -1 ) round-trips through int cleanly and would
        // pass. cmp_equal compares sign-aware and catches it.
        assert( std::cmp_equal( result, value ) && "narrow_cast lost information" );
    }
    else
    {
        assert( static_cast<From>( result ) == value && "narrow_cast lost information" );
    }

    return result;
}

} // namespace keel
