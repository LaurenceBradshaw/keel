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

std::optional<Rvalue> identity( const Rvalue& value, const Type_table& types, Literal_pool& literals )
{
    if( value.kind != Rvalue_kind::Binary )
    {
        return std::nullopt;
    }

    const Type_kind kind = types.get( value.a.type ).kind;

    const auto whole = []( const Operand& x )
    { return x.kind == Operand_kind::Copy && !x.place.is_global() && x.place.num_projections == 0; };

    // Never a float, which NaN makes unequal to itself.
    if( whole( value.a ) && whole( value.b ) && value.a.place.local == value.b.place.local &&
        ( kind == Type_kind::Int || kind == Type_kind::Bool || kind == Type_kind::Pointer ) )
    {
        switch( value.op )
        {
        case Token_kind::Equal_equal:
        case Token_kind::Less_equal:
        case Token_kind::Greater_equal:
            return value_of( 1, value.type, types, literals );

        case Token_kind::Bang_equal:
        case Token_kind::Less:
        case Token_kind::Greater:
            return value_of( 0, value.type, types, literals );

        default:
            break;
        }
    }

    // Not a float's: `-0.0 + 0` is `0.0`.
    if( kind != Type_kind::Int )
    {
        return std::nullopt;
    }

    const auto is = [&]( const Operand& x, u64 n )
    { return x.kind == Operand_kind::Constant && x.constant.is_valid() && literals.integer( x.constant ) == n; };

    switch( value.op )
    {
    case Token_kind::Plus:
        if( is( value.b, 0 ) )
        {
            return use( value.a );
        }
        if( is( value.a, 0 ) )
        {
            return use( value.b );
        }
        break;

    case Token_kind::Minus:
        if( is( value.b, 0 ) )
        {
            return use( value.a );
        }
        break;

    case Token_kind::Star:
        if( is( value.b, 1 ) )
        {
            return use( value.a );
        }
        if( is( value.a, 1 ) )
        {
            return use( value.b );
        }
        break;

    default:
        break;
    }

    return std::nullopt;
}

// Combined wrapped at the type's width: exact without overflow checks, and with them past every check.
std::optional<Rvalue> combined( const Rvalue& outer, const Rvalue& inner, const Type_table& types, Literal_pool& literals )
{
    if( outer.kind != Rvalue_kind::Binary || inner.kind != Rvalue_kind::Binary || outer.type != inner.type ||
        inner.a.type != inner.type || types.get( outer.type ).kind != Type_kind::Int || outer.a.kind != Operand_kind::Copy )
    {
        return std::nullopt;
    }

    const auto additive = []( Token_kind op ) { return op == Token_kind::Plus || op == Token_kind::Minus; };

    const auto known = []( const Operand& x ) { return x.kind == Operand_kind::Constant && x.constant.is_valid(); };

    if( !additive( outer.op ) || !additive( inner.op ) || !known( outer.b ) || !known( inner.b ) )
    {
        return std::nullopt;
    }

    const Integer integer = *integer_type( types, outer.type );
    const u64     c1      = literals.integer( inner.b.constant );
    const u64     c2      = literals.integer( outer.b.constant );

    // What `x` is shifted by.
    u64 net = inner.op == Token_kind::Plus ? c1 : truncate( ~c1 + 1, integer.width );

    net = *arithmetic( outer.op, net, c2, integer );

    if( net == 0 )
    {
        return use( inner.a );
    }

    // A negative is a subtraction, as an operand has no negative form.
    const u64 top = 1ull << ( integer.width - 1 );
    if( net & top )
    {
        // A signed minimum has no positive magnitude.
        const u64 magnitude = truncate( ~net + 1, integer.width );
        if( magnitude == net && integer.is_signed )
        {
            return std::nullopt;
        }

        return binary( Token_kind::Minus, inner.a, constant( literals.add_integer( magnitude ), outer.type ), outer.type );
    }

    return binary( Token_kind::Plus, inner.a, constant( literals.add_integer( net ), outer.type ), outer.type );
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

TEST_CASE( "fold_constants_folds_a_local_compared_with_itself", "[opt][fold]" )
{
    const Type_table types;
    Literal_pool     literals;
    const Type_id    i32     = types.integer( 32, true );
    const Type_id    boolean = types.builtin( Type_kind::Bool );
    const Operand    x       = copy( Place { .local = Local_id { 1 } }, i32 );

    const std::optional<Rvalue> equal = identity( binary( Token_kind::Equal_equal, x, x, boolean ), types, literals );
    REQUIRE( equal );
    REQUIRE( literals.integer( equal->a.constant ) == 1 );

    const std::optional<Rvalue> below = identity( binary( Token_kind::Less, x, x, boolean ), types, literals );
    REQUIRE( below );
    REQUIRE( literals.integer( below->a.constant ) == 0 );
}

// A NaN is unequal to itself.
TEST_CASE( "fold_constants_keeps_a_float_compared_with_itself", "[opt][fold]" )
{
    const Type_table types;
    Literal_pool     literals;
    const Type_id    f64 = types.floating( 64 );
    const Operand    x   = copy( Place { .local = Local_id { 1 } }, f64 );

    REQUIRE( !identity( binary( Token_kind::Equal_equal, x, x, types.builtin( Type_kind::Bool ) ), types, literals ) );
}

// `_1.a == _1.a` would fold too, but only a whole local is judged.
TEST_CASE( "fold_constants_keeps_a_projected_place_compared_with_itself", "[opt][fold]" )
{
    const Type_table types;
    Literal_pool     literals;
    const Type_id    i32 = types.integer( 32, true );
    const Operand    x   = copy( Place { .local = Local_id { 1 }, .num_projections = 1 }, i32 );

    REQUIRE( !identity( binary( Token_kind::Equal_equal, x, x, types.builtin( Type_kind::Bool ) ), types, literals ) );
}

TEST_CASE( "fold_constants_folds_an_added_zero", "[opt][fold]" )
{
    const Type_table types;
    Literal_pool     literals;
    const Type_id    i32  = types.integer( 32, true );
    const Operand    x    = copy( Place { .local = Local_id { 1 } }, i32 );
    const Operand    zero = constant( literals.add_integer( 0 ), i32 );

    const std::optional<Rvalue> sum = identity( binary( Token_kind::Plus, zero, x, i32 ), types, literals );
    REQUIRE( sum );
    REQUIRE( sum->kind == Rvalue_kind::Use );
    REQUIRE( sum->a.place.local == x.place.local );

    REQUIRE( !identity( binary( Token_kind::Minus, zero, x, i32 ), types, literals ) );
}

// `(x + 3) + 4` is `x + 7`, `(x + 3) - 5` is `x - 2`, and `(x - 3) + 3` is `x`.
TEST_CASE( "fold_constants_combines_two_sums", "[opt][fold]" )
{
    const Type_table types;
    Literal_pool     literals;
    const Type_id    i32 = types.integer( 32, true );
    const Operand    x   = copy( Place { .local = Local_id { 1 } }, i32 );
    const Operand    t   = copy( Place { .local = Local_id { 2 } }, i32 );
    const auto       c   = [&]( u64 n ) { return constant( literals.add_integer( n ), i32 ); };

    const std::optional<Rvalue> up =
        combined( binary( Token_kind::Plus, t, c( 4 ), i32 ), binary( Token_kind::Plus, x, c( 3 ), i32 ), types, literals );
    REQUIRE( up );
    REQUIRE( up->op == Token_kind::Plus );
    REQUIRE( up->a.place.local == x.place.local );
    REQUIRE( literals.integer( up->b.constant ) == 7 );

    const std::optional<Rvalue> down =
        combined( binary( Token_kind::Minus, t, c( 5 ), i32 ), binary( Token_kind::Plus, x, c( 3 ), i32 ), types, literals );
    REQUIRE( down );
    REQUIRE( down->op == Token_kind::Minus );
    REQUIRE( literals.integer( down->b.constant ) == 2 );

    const std::optional<Rvalue> none =
        combined( binary( Token_kind::Plus, t, c( 3 ), i32 ), binary( Token_kind::Minus, x, c( 3 ), i32 ), types, literals );
    REQUIRE( none );
    REQUIRE( none->kind == Rvalue_kind::Use );
    REQUIRE( none->a.place.local == x.place.local );
}

// A signed minimum has no positive constant to subtract, and `*` is not a sum; an unsigned minimum is.
TEST_CASE( "fold_constants_keeps_a_combined_minimum", "[opt][fold]" )
{
    const Type_table types;
    Literal_pool     literals;
    const Type_id    i32        = types.integer( 32, true );
    const Type_id    unsigned32 = types.integer( 32, false );
    const auto       c          = [&]( u64 n, Type_id type ) { return constant( literals.add_integer( n ), type ); };
    const auto       sum        = [&]( Token_kind op, u32 local, u64 n, Type_id type )
    { return binary( op, copy( Place { .local = Local_id { local } }, type ), c( n, type ), type ); };

    REQUIRE( !combined( sum( Token_kind::Plus, 2, 1, i32 ), sum( Token_kind::Plus, 1, 2147483647, i32 ), types, literals ) );
    REQUIRE( !combined(
        binary( Token_kind::Star, copy( Place { .local = Local_id { 2 } }, i32 ), c( 2, i32 ), i32 ),
        sum( Token_kind::Plus, 1, 3, i32 ),
        types,
        literals
    ) );

    const std::optional<Rvalue> wrapped = combined(
        sum( Token_kind::Plus, 2, 1, unsigned32 ), sum( Token_kind::Plus, 1, 2147483647, unsigned32 ), types, literals
    );
    REQUIRE( wrapped );
    REQUIRE( wrapped->op == Token_kind::Minus );
    REQUIRE( literals.integer( wrapped->b.constant ) == 2147483648u );
}

} // namespace keel
#endif
