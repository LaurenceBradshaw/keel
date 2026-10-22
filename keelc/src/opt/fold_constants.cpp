// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/fold_constants.h"
#include <optional>

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

std::optional<bool> against_zero( const Rvalue& value, const Type_table& types, const Literal_pool& literals )
{
    const std::optional<Integer> integer = integer_type( types, value.a.type );
    if( value.kind != Rvalue_kind::Binary || !integer || integer->is_signed )
    {
        return std::nullopt;
    }

    const auto is_zero = [&]( const Operand& operand )
    { return operand.kind == Operand_kind::Constant && literals.integer( operand.constant ) == 0; };

    if( is_zero( value.b ) && ( value.op == Token_kind::Greater_equal || value.op == Token_kind::Less ) )
    {
        return value.op == Token_kind::Greater_equal;
    }

    if( is_zero( value.a ) && ( value.op == Token_kind::Less_equal || value.op == Token_kind::Greater ) )
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

Rvalue value_of( u64 bits, Integer integer, Type_id type, Literal_pool& literals )
{
    if( integer.is_signed && ( bits & ( 1ull << ( integer.width - 1 ) ) ) )
    {
        const u64 magnitude = truncate( ~bits + 1, integer.width );
        return unary( Token_kind::Minus, constant( literals.add_integer( magnitude ), type ), type );
    }

    return use( constant( literals.add_integer( bits ), type ) );
}

Rvalue truth( bool b, Type_id type, Literal_pool& literals )
{
    return use( constant( literals.add_integer( b ? 1 : 0 ), type ) );
}

std::optional<Rvalue> fold( const Rvalue& value, const Type_table& types, Literal_pool& literals )
{
    // Other kinds may leave `a` a default operand, with no type.
    if( value.kind != Rvalue_kind::Binary && value.kind != Rvalue_kind::Overflows && value.kind != Rvalue_kind::Unary &&
        value.kind != Rvalue_kind::Cast )
    {
        return std::nullopt;
    }

    if( const std::optional<bool> decided = against_zero( value, types, literals ) )
    {
        return truth( *decided, value.type, literals );
    }

    // Checked before the read: a float literal's id indexes another array of the pool.
    const std::optional<Integer> integer = integer_type( types, value.a.type );
    if( value.a.kind != Operand_kind::Constant || !integer )
    {
        return std::nullopt;
    }

    const u64 a = literals.integer( value.a.constant );

    switch( value.kind )
    {
    // A shift's count may be another integer type; only its bits matter.
    case Rvalue_kind::Binary:
    case Rvalue_kind::Overflows:
    {
        if( value.b.kind != Operand_kind::Constant )
        {
            return std::nullopt;
        }

        const u64 b = literals.integer( value.b.constant );

        if( value.kind == Rvalue_kind::Overflows )
        {
            return truth( overflows( value.op, a, b, *integer ), value.type, literals );
        }

        if( const std::optional<bool> result = comparison( value.op, a, b, *integer ) )
        {
            return truth( *result, value.type, literals );
        }

        if( const std::optional<u64> result = arithmetic( value.op, a, b, *integer ) )
        {
            return value_of( *result, *integer, value.type, literals );
        }

        return std::nullopt;
    }

    case Rvalue_kind::Unary:
        switch( value.op )
        {
        case Token_kind::Bang:
            return truth( a == 0, value.type, literals );

        case Token_kind::Tilde:
            return value_of( truncate( ~a, integer->width ), *integer, value.type, literals );

        // A signed `-const m` is the folded form itself; folding it again would never end.
        case Token_kind::Minus:
            if( integer->is_signed )
            {
                return std::nullopt;
            }

            return value_of( truncate( 0 - a, integer->width ), *integer, value.type, literals );

        default:
            return std::nullopt;
        }

    // A constant is never negative, so truncating is the whole conversion. Not to `bool`, which
    // would test against zero.
    case Rvalue_kind::Cast:
    {
        const std::optional<Integer> to = integer_type( types, value.type );
        if( !to || types.get( value.type ).kind != Type_kind::Int )
        {
            return std::nullopt;
        }

        return value_of( truncate( a, to->width ), *to, value.type, literals );
    }

    default:
        return std::nullopt;
    }
}

} // namespace

bool fold_constants( Function& func, const Type_table& types, Literal_pool& literals )
{
    bool changed = false;
    for( Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        if( const std::optional<Rvalue> folded = fold( statement.value, types, literals ) )
        {
            statement.value = *folded;
            changed         = true;
        }
    }

    return changed;
}
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

constexpr Integer k_i32 { .width = 32, .is_signed = true };
constexpr Integer k_i64 { .width = 64, .is_signed = true };

// Optimised as a release build emits it; the function under test is first.
struct Folded : Compiled
{
    explicit Folded( std::string_view source )
        : Compiled( std::string( source ) + "i32 main() { return 0; }\n", Stage::Elaborated, Build { .optimise = true } )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }
};

} // namespace

// `2147483647 + 1` is the minimum, written as lowering writes `-2147483648`.
TEST_CASE( "fold_constants_wraps_a_signed_sum", "[opt][fold]" )
{
    const Type_table types;
    Literal_pool     literals;
    const Type_id    i32 = types.integer( 32, true );

    const std::optional<Rvalue> folded = fold(
        binary(
            Token_kind::Plus,
            constant( literals.add_integer( 2147483647 ), i32 ),
            constant( literals.add_integer( 1 ), i32 ),
            i32
        ),
        types,
        literals
    );

    REQUIRE( folded );
    REQUIRE( folded->kind == Rvalue_kind::Unary );
    REQUIRE( folded->op == Token_kind::Minus );
    REQUIRE( literals.integer( folded->a.constant ) == 2147483648u );
}

// `-const 2` is the folded form; folding it again would never end.
TEST_CASE( "fold_constants_keeps_a_negative_constant", "[opt][fold]" )
{
    Folded p( "i32 f() { i32 a = 3; return a - 5; }\n" );
    REQUIRE( p.clean() );

    const auto negative = std::ranges::find_if(
        p.first().statements,
        []( const Statement& s )
        { return s.kind == Statement_kind::Assign && s.value.kind == Rvalue_kind::Unary && s.value.op == Token_kind::Minus; }
    );
    REQUIRE( negative != p.first().statements.end() );
    REQUIRE( p.literals.integer( negative->value.a.constant ) == 2 );
    REQUIRE( !fold( negative->value, p.types.table(), p.literals ) );
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

// Each fold makes the next pair: `c - 1` is `13` only after `b` and `c` fold in turn.
TEST_CASE( "fold_constants_reaches_a_fixpoint_with_propagation", "[opt][fold]" )
{
    Folded p( "i32 f() { i32 a = 3; i32 b = a + 4; i32 c = b * 2; return c - 1; }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );

    for( const Statement& s : p.first().statements )
    {
        REQUIRE( ( s.kind != Statement_kind::Assign || s.value.kind != Rvalue_kind::Binary ) );
    }

    const bool thirteen = std::ranges::any_of(
        p.first().statements,
        [&]( const Statement& s )
        {
            return s.kind == Statement_kind::Assign && s.value.kind == Rvalue_kind::Use &&
                   s.value.a.kind == Operand_kind::Constant && p.literals.integer( s.value.a.constant ) == 13;
        }
    );
    REQUIRE( thirteen );

    Function again = p.first();
    REQUIRE( !fold_constants( again, p.types.table(), p.literals ) );
}

} // namespace keel
#endif
