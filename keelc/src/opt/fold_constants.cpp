// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/fold_constants.h"

namespace keel
{

namespace
{
struct Integer
{
    u8   width;
    bool is_signed;
};

std::optional<Integer> integer_type( const Type_table& types, Type_id type )
{
    const Type& t = types.get( type );

    if( t.kind == Type_kind::Int )
    {
        return Integer { .width = t.width, .is_signed = t.is_signed };
    }

    if( t.kind == Type_kind::Bool )
    {
        return Integer { .width = 8, .is_signed = false };
    }

    return std::nullopt;
}

u64 truncate( u64 bits, u8 width )
{
    if( width == 64 )
    {
        return bits;
    }

    return bits & ( ( 1ull << width ) - 1 );
}

i64 as_signed( u64 bits, u8 width )
{
    if( width == 64 )
    {
        return static_cast<i64>( bits );
    }

    const u64 sign_bit = 1ull << ( width - 1 );
    if( bits & sign_bit )
    {
        const u64 mask = ~0ull << width;
        return static_cast<i64>( bits | mask );
    }

    return static_cast<i64>( bits );
}

std::optional<u64> arithmetic( Token_kind op, u64 a, u64 b, Integer type )
{
    switch( op )
    {
    case Token_kind::Plus:
        return truncate( a + b, type.width );

    case Token_kind::Minus:
        return truncate( a - b, type.width );

    case Token_kind::Star:
        return truncate( a * b, type.width );

    case Token_kind::Amp:
        return truncate( a & b, type.width );

    case Token_kind::Caret:
        return truncate( a ^ b, type.width );

    case Token_kind::Pipe:
        return truncate( a | b, type.width );

    case Token_kind::Less_less:
        return truncate( a << ( b & ( type.width - 1 ) ), type.width );

    case Token_kind::Greater_greater:
        if( type.is_signed )
        {
            return truncate( static_cast<u64>( as_signed( a, type.width ) >> ( b & ( type.width - 1 ) ) ), type.width );
        }
        else
        {
            return truncate( a >> ( b & ( type.width - 1 ) ), type.width );
        }

    case Token_kind::Slash:
    case Token_kind::Percent:
    {
        // A zero divisor stays, so the check before it still panics.
        if( truncate( b, type.width ) == 0 )
        {
            return std::nullopt;
        }

        if( !type.is_signed )
        {
            return op == Token_kind::Slash ? a / b : a % b;
        }

        const i64 sa = as_signed( a, type.width );
        const i64 sb = as_signed( b, type.width );

        // Taken first: `MIN / -1` is undefined in C++, but wraps to `MIN` in Keel.
        if( sb == -1 )
        {
            return op == Token_kind::Slash ? truncate( 0 - a, type.width ) : 0;
        }

        return truncate( static_cast<u64>( op == Token_kind::Slash ? sa / sb : sa % sb ), type.width );
    }

    default:
        return std::nullopt;
    }
}

std::optional<bool> comparison( Token_kind op, u64 a, u64 b, Integer type )
{
    switch( op )
    {
    case Token_kind::Equal_equal:
        return a == b;

    case Token_kind::Bang_equal:
        return a != b;

    case Token_kind::Less:
        if( type.is_signed )
        {
            return as_signed( a, type.width ) < as_signed( b, type.width );
        }
        else
        {
            return a < b;
        }

    case Token_kind::Greater:
        if( type.is_signed )
        {
            return as_signed( a, type.width ) > as_signed( b, type.width );
        }
        else
        {
            return a > b;
        }

    case Token_kind::Less_equal:
        if( type.is_signed )
        {
            return as_signed( a, type.width ) <= as_signed( b, type.width );
        }
        else
        {
            return a <= b;
        }

    case Token_kind::Greater_equal:
        if( type.is_signed )
        {
            return as_signed( a, type.width ) >= as_signed( b, type.width );
        }
        else
        {
            return a >= b;
        }

    default:
        return std::nullopt;
    }
}

std::optional<bool> against_zero( const Rvalue& value, std::optional<u64> a, std::optional<u64> b, const Type_table& types )
{
    const std::optional<Integer> integer = integer_type( types, value.a.type );
    if( value.kind != Rvalue_kind::Binary || !integer || integer->is_signed )
    {
        return std::nullopt;
    }

    const auto is_zero = [&]( const std::optional<u64>& x ) { return x && *x == 0; };

    if( is_zero( b ) && ( value.op == Token_kind::Greater_equal || value.op == Token_kind::Less ) )
    {
        return value.op == Token_kind::Greater_equal;
    }

    if( is_zero( a ) && ( value.op == Token_kind::Less_equal || value.op == Token_kind::Greater ) )
    {
        return value.op == Token_kind::Less_equal;
    }

    return std::nullopt;
}

// The builtins the emitter writes for `overflows`, so the fold and the run agree.
template <typename T>
bool overflows_as( Token_kind op, u64 a, u64 b )
{
    const T x = static_cast<T>( a );
    const T y = static_cast<T>( b );
    T       result;

    switch( op )
    {
    case Token_kind::Plus:
        return __builtin_add_overflow( x, y, &result );

    case Token_kind::Minus:
        return __builtin_sub_overflow( x, y, &result );

    default:
        return __builtin_mul_overflow( x, y, &result );
    }
}

bool overflows( Token_kind op, u64 a, u64 b, Integer type )
{
    switch( type.width )
    {
    case 8:
        return type.is_signed ? overflows_as<i8>( op, a, b ) : overflows_as<u8>( op, a, b );

    case 16:
        return type.is_signed ? overflows_as<i16>( op, a, b ) : overflows_as<u16>( op, a, b );

    case 32:
        return type.is_signed ? overflows_as<i32>( op, a, b ) : overflows_as<u32>( op, a, b );

    default:
        return type.is_signed ? overflows_as<i64>( op, a, b ) : overflows_as<u64>( op, a, b );
    }
}

} // namespace

std::optional<u64> evaluate( const Rvalue& value, std::optional<u64> a, std::optional<u64> b, const Type_table& types )
{
    // Other kinds may leave `a` a default operand, with no type.
    if( value.kind != Rvalue_kind::Use && value.kind != Rvalue_kind::Binary && value.kind != Rvalue_kind::Overflows &&
        value.kind != Rvalue_kind::Unary && value.kind != Rvalue_kind::Cast )
    {
        return std::nullopt;
    }

    if( const std::optional<bool> decided = against_zero( value, a, b, types ) )
    {
        return *decided ? 1 : 0;
    }

    const std::optional<Integer> integer = integer_type( types, value.a.type );
    if( !a || !integer )
    {
        return std::nullopt;
    }

    switch( value.kind )
    {
    case Rvalue_kind::Use:
        return *a;

    // A shift's count may be another integer type; only its bits matter.
    case Rvalue_kind::Binary:
    case Rvalue_kind::Overflows:
    {
        if( !b )
        {
            return std::nullopt;
        }

        if( value.kind == Rvalue_kind::Overflows )
        {
            return overflows( value.op, *a, *b, *integer ) ? 1 : 0;
        }

        if( const std::optional<bool> result = comparison( value.op, *a, *b, *integer ) )
        {
            return *result ? 1 : 0;
        }

        return arithmetic( value.op, *a, *b, *integer );
    }

    case Rvalue_kind::Unary:
        switch( value.op )
        {
        case Token_kind::Bang:
            return *a == 0 ? 1 : 0;

        case Token_kind::Tilde:
            return truncate( ~*a, integer->width );

        case Token_kind::Minus:
            return truncate( 0 - *a, integer->width );

        default:
            return std::nullopt;
        }

    // Not to `bool`, which would test against zero.
    case Rvalue_kind::Cast:
    {
        const std::optional<Integer> to = integer_type( types, value.type );
        if( !to || types.get( value.type ).kind != Type_kind::Int )
        {
            return std::nullopt;
        }

        return convert( *a, value.a.type, value.type, types );
    }

    default:
        return std::nullopt;
    }
}

std::optional<u64> convert( u64 bits, Type_id from, Type_id to, const Type_table& types )
{
    if( ( types.get( from ).kind != Type_kind::Int && types.get( from ).kind != Type_kind::Bool ) ||
        ( types.get( to ).kind != Type_kind::Int && types.get( to ).kind != Type_kind::Bool ) )
    {
        return std::nullopt;
    }

    const std::optional<Integer> integer_to   = integer_type( types, to );
    const std::optional<Integer> integer_from = integer_type( types, from );

    if( !integer_to || !integer_from )
    {
        return std::nullopt;
    }

    if( integer_from->is_signed )
    {
        return truncate( static_cast<u64>( as_signed( bits, integer_from->width ) ), integer_to->width );
    }

    return truncate( bits, integer_to->width );
}

Rvalue value_of( u64 bits, Type_id type, const Type_table& types, Literal_pool& literals )
{
    const Integer integer = *integer_type( types, type );
    if( integer.is_signed && ( bits & ( 1ull << ( integer.width - 1 ) ) ) )
    {
        const u64 magnitude = truncate( ~bits + 1, integer.width );
        return unary( Token_kind::Minus, constant( literals.add_integer( magnitude ), type ), type );
    }

    return use( constant( literals.add_integer( bits ), type ) );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{
namespace
{

constexpr Integer k_i32 { .width = 32, .is_signed = true };
constexpr Integer k_i64 { .width = 64, .is_signed = true };

} // namespace

// `2147483647 + 1` is the minimum, written as lowering writes `-2147483648`.
TEST_CASE( "fold_constants_wraps_a_signed_sum", "[opt][fold]" )
{
    const Type_table types;
    Literal_pool     literals;
    const Type_id    i32 = types.integer( 32, true );

    const Rvalue sum = binary( Token_kind::Plus, constant( Literal_id {}, i32 ), constant( Literal_id {}, i32 ), i32 );
    const std::optional<u64> bits = evaluate( sum, 2147483647, 1, types );
    REQUIRE( bits == 0x8000'0000u );

    const Rvalue folded = value_of( *bits, i32, types, literals );
    REQUIRE( folded.kind == Rvalue_kind::Unary );
    REQUIRE( folded.op == Token_kind::Minus );
    REQUIRE( literals.integer( folded.a.constant ) == 2147483648u );
}

// `-4` as an `i32` is `-4` as an `i64`, not `4294967292`; an unsigned source zero-extends.
TEST_CASE( "fold_constants_sign_extends_a_cast", "[opt][fold]" )
{
    const Type_table types;
    const Type_id    i32 = types.integer( 32, true );
    const Type_id    u32 = types.integer( 32, false );
    const Type_id    i64 = types.integer( 64, true );

    REQUIRE( evaluate( cast_to( constant( Literal_id {}, i32 ), i64 ), 0xFFFF'FFFCu, std::nullopt, types ) == ~0ull - 3 );
    REQUIRE( evaluate( cast_to( constant( Literal_id {}, u32 ), i64 ), 0xFFFF'FFFCu, std::nullopt, types ) == 0xFFFF'FFFCu );
}

// The check before a zero divisor panics, so the division stays.
TEST_CASE( "fold_constants_keeps_a_zero_divisor", "[opt][fold]" )
{
    REQUIRE( !arithmetic( Token_kind::Slash, 7, 0, k_i32 ) );
    REQUIRE( !arithmetic( Token_kind::Percent, 7, 0, k_i32 ) );
}

// Undefined in C++, so taken before the division: `MIN / -1` wraps to `MIN`, and `MIN % -1` is 0.
TEST_CASE( "fold_constants_divides_min_by_minus_one", "[opt][fold]" )
{
    REQUIRE( arithmetic( Token_kind::Slash, 0x8000'0000u, 0xFFFF'FFFFu, k_i32 ) == 0x8000'0000u );
    REQUIRE( arithmetic( Token_kind::Percent, 0x8000'0000u, 0xFFFF'FFFFu, k_i32 ) == 0u );
    REQUIRE( arithmetic( Token_kind::Slash, 1ull << 63, ~0ull, k_i64 ) == 1ull << 63 );
    REQUIRE( arithmetic( Token_kind::Slash, 0xFFFF'FFF9u, 2, k_i32 ) == 0xFFFF'FFFDu );
}

} // namespace keel
#endif
