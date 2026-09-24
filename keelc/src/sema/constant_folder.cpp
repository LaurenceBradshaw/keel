#include "sema/constant_folder.h"
#include <fmt/format.h>
#include <limits>

namespace keel
{

namespace sema
{

namespace
{

constexpr Folded not_constant()
{
    return Folded {};
}

constexpr Folded too_large()
{
    return Folded { true, true, Constant {} };
}

// Zero has no sign. Without this `0 - 0` folds to a negative zero, which no type holds.
constexpr Folded folded( u64 magnitude, bool negative )
{
    return Folded { true, false, Constant { magnitude, magnitude == 0 ? false : negative } };
}

// Bitwise operators are defined on the *representation*, and sign-magnitude has no bit pattern of
// its own - so folding one means converting to two's complement in the type's width, operating,
// and converting back. That is why these need the type where the arithmetic ones do not.
u64 to_bits( Constant value, u8 width )
{
    const u64 mask = width == 64 ? ~0ull : ( 1ull << width ) - 1;

    return ( value.negative ? ~value.magnitude + 1 : value.magnitude ) & mask;
}

Folded from_bits( u64 bits, u8 width, bool is_signed )
{
    const u64 mask = width == 64 ? ~0ull : ( 1ull << width ) - 1;

    bits &= mask;

    // The sign bit is only a sign in a signed type. In an unsigned one the same pattern is just a
    // large positive value, which is why ~0 is -1 as an i32 and the maximum as a u32.
    const u64 sign = width == 64 ? 1ull << 63 : 1ull << ( width - 1 );

    if( is_signed && ( bits & sign ) != 0 )
    {
        return folded( ( ~bits + 1 ) & mask, true );
    }

    return folded( bits, false );
}

// Negative sorts below positive whatever the magnitudes; within one sign the magnitude decides,
// reversed when both are negative.
bool less_than( Constant a, Constant b )
{
    if( a.negative != b.negative )
    {
        return a.negative;
    }

    return a.negative ? a.magnitude > b.magnitude : a.magnitude < b.magnitude;
}

bool equals( Constant a, Constant b )
{
    return a.magnitude == b.magnitude && a.negative == b.negative;
}

Folded add_constants( Constant a, Constant b )
{
    if( a.negative == b.negative )
    {
        if( a.magnitude > std::numeric_limits<u64>::max() - b.magnitude )
        {
            return too_large();
        }

        return folded( a.magnitude + b.magnitude, a.negative );
    }

    // Opposite signs: the larger magnitude decides both the size and the sign of the answer.
    return a.magnitude >= b.magnitude ? folded( a.magnitude - b.magnitude, a.negative )
                                      : folded( b.magnitude - a.magnitude, b.negative );
}

Folded multiply_constants( Constant a, Constant b )
{
    if( a.magnitude != 0 && b.magnitude > std::numeric_limits<u64>::max() / a.magnitude )
    {
        return too_large();
    }

    return folded( a.magnitude * b.magnitude, a.negative != b.negative );
}

} // namespace

// A file-scope initialiser has to be something C accepts as a constant expression too, so the
// emitter can print it rather than running code before main. Literals and arithmetic over them
// qualify; anything that loads, calls or takes an address does not. `&&` and `||` are out
// deliberately - they lower to control flow, and they buy nothing in an initialiser.
bool Constant_folder::is_constant_expression( Node_id id ) const
{
    if( !id.is_valid() )
    {
        return false;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Int_literal:
    case Node_kind::Float_literal:
    case Node_kind::Bool_literal:
    case Node_kind::Char_literal:
    case Node_kind::Null_literal:
        return true;

    case Node_kind::Unary_expr:
        // `&x` is an address and `*p` a load; neither is a value known here.
        switch( static_cast<Token_kind>( ast_.aux( id ) ) )
        {
        case Token_kind::Minus:
        case Token_kind::Plus:
        case Token_kind::Tilde:
        case Token_kind::Bang:
            return is_constant_expression( ast_.child( id, 0 ) );

        default:
            return false;
        }

    case Node_kind::Binary_expr:
    {
        const Token_kind op = static_cast<Token_kind>( ast_.aux( id ) );

        if( op == Token_kind::Amp_amp || op == Token_kind::Pipe_pipe )
        {
            return false;
        }

        return is_constant_expression( ast_.child( id, 0 ) ) && is_constant_expression( ast_.child( id, 1 ) );
    }

    case Node_kind::Cast_expr:
        return is_constant_expression( ast_.child( id, 1 ) );

    default:
        return false;
    }
}

// Only + - * / % << >> fold. `&`, `|`, `^` and `~` cannot take a value outside the type their
// operands came from, so there is nothing for them to overflow and nothing here to check - saying
// "not constant" for those is the correct answer, not a shortcut.
Folded Constant_folder::fold_integer( Node_id id, Type_id known ) const
{
    // Only the node being folded needs the hint: its children were checked first, so their own
    // types are recorded by the time the recursion reaches them.
    const auto width_type = [&]() -> Type_id { return known.is_valid() ? known : types_.type_of( id ); };

    if( !id.is_valid() )
    {
        return not_constant();
    }

    // A node already reported as wrong contributes nothing. Folding through it would report the
    // same mistake again from every operation that encloses it.
    //
    // is_valid() first, and it is not redundant: is_error() answers true for an unrecorded type
    // too, and the node being folded is unrecorded by definition - record_constant runs before the
    // record. Without this the guard rejects every node it is asked about.
    if( types_.type_of( id ).is_valid() && table_.is_error( types_.type_of( id ) ) )
    {
        return not_constant();
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Char_literal: // a code point is an integer value
    case Node_kind::Int_literal:
    {
        const Literal_id value { ast_.aux( id ) };

        // A literal the lexer could not scan has no value recorded. It reported there.
        return value.is_valid() ? folded( literal_pool_.integer( value ), false ) : not_constant();
    }

    // Neither carries a Literal_id: a bool's value is in aux, and `nullptr` records nothing at
    // all. Both are integers here, which is also how the lowerer spells them.
    case Node_kind::Bool_literal:
        return folded( ast_.aux( id ) != 0 ? 1 : 0, false );

    case Node_kind::Null_literal:
        return folded( 0, false );

    case Node_kind::Unary_expr:
    {
        const Folded operand = fold_integer( ast_.child( id, 0 ) );

        if( !operand.constant || operand.overflowed )
        {
            return operand;
        }

        switch( static_cast<Token_kind>( ast_.aux( id ) ) )
        {
        case Token_kind::Minus:
            return folded( operand.value.magnitude, !operand.value.negative );

        case Token_kind::Plus:
            return operand;

        case Token_kind::Tilde:
        {
            const Type_id operation = width_type();

            if( !operation.is_valid() || !table_.is_integer( operation ) )
            {
                return not_constant();
            }

            const Type& described = table_.get( operation );

            return from_bits( ~to_bits( operand.value, described.width ), described.width, described.is_signed );
        }

        default:
            return not_constant();
        }
    }

    // Either a widening `cast` or a `wrap` - narrowing `cast` is refused until there is something
    // to trap with - so this is a reduction modulo the target's width either way.
    case Node_kind::Cast_expr:
    {
        const Folded operand = fold_integer( ast_.child( id, 1 ) );

        if( !operand.constant || operand.overflowed )
        {
            return operand;
        }

        const Type_id target = width_type();

        if( !target.is_valid() || !table_.is_integer( target ) )
        {
            return not_constant();
        }

        const Type& described = table_.get( target );

        return from_bits( to_bits( operand.value, described.width ), described.width, described.is_signed );
    }

    case Node_kind::Binary_expr:
    {
        const Folded left  = fold_integer( ast_.child( id, 0 ) );
        const Folded right = fold_integer( ast_.child( id, 1 ) );

        if( !left.constant || !right.constant )
        {
            return not_constant();
        }

        if( left.overflowed || right.overflowed )
        {
            return too_large();
        }

        const Constant a = left.value;
        const Constant b = right.value;

        switch( static_cast<Token_kind>( ast_.aux( id ) ) )
        {
        case Token_kind::Plus:
            return add_constants( a, b );

        case Token_kind::Minus:
            return add_constants( a, Constant { b.magnitude, b.magnitude != 0 && !b.negative } );

        case Token_kind::Star:
            return multiply_constants( a, b );

        // Division truncates toward zero, and the remainder takes the sign of the dividend - which
        // is what sign-magnitude does on its own. check_constant reports the zero divisor.
        case Token_kind::Slash:
            return b.magnitude == 0 ? not_constant() : folded( a.magnitude / b.magnitude, a.negative != b.negative );

        case Token_kind::Percent:
            return b.magnitude == 0 ? not_constant() : folded( a.magnitude % b.magnitude, a.negative );

        case Token_kind::Less_less:
            // An out-of-range count is reported by check_constant; 1ull << 64 is undefined here
            // too, so the guard protects the compiler as much as the program.
            if( b.negative || b.magnitude >= 64 )
            {
                return not_constant();
            }

            return multiply_constants( a, Constant { 1ull << b.magnitude, false } );

        case Token_kind::Greater_greater:
            if( b.negative || b.magnitude >= 64 || a.negative )
            {
                return not_constant();
            }

            return folded( a.magnitude >> b.magnitude, false );

        // A comparison yields bool, which is the integer 0 or 1 here.
        case Token_kind::Less:
            return folded( less_than( a, b ) ? 1 : 0, false );

        case Token_kind::Greater:
            return folded( less_than( b, a ) ? 1 : 0, false );

        case Token_kind::Less_equal:
            return folded( less_than( b, a ) ? 0 : 1, false );

        case Token_kind::Greater_equal:
            return folded( less_than( a, b ) ? 0 : 1, false );

        case Token_kind::Equal_equal:
            return folded( equals( a, b ) ? 1 : 0, false );

        case Token_kind::Bang_equal:
            return folded( equals( a, b ) ? 0 : 1, false );

        case Token_kind::Amp:
        case Token_kind::Pipe:
        case Token_kind::Caret:
        {
            // The width the operation happens in, which the node carries. Without it there is no
            // bit pattern to work on.
            const Type_id operation = width_type();

            if( !operation.is_valid() || !table_.is_integer( operation ) )
            {
                return not_constant();
            }

            const Type& described  = table_.get( operation );
            const u64   left_bits  = to_bits( a, described.width );
            const u64   right_bits = to_bits( b, described.width );

            const Token_kind op = static_cast<Token_kind>( ast_.aux( id ) );

            const u64 result = op == Token_kind::Amp    ? left_bits & right_bits
                               : op == Token_kind::Pipe ? left_bits | right_bits
                                                        : left_bits ^ right_bits;

            return from_bits( result, described.width, described.is_signed );
        }

        default:
            return not_constant();
        }
    }

    default:
        return not_constant();
    }
}

std::optional<f64> Constant_folder::fold_float( Node_id id ) const
{
    // is_valid() first, for the same reason as fold_integer: an unrecorded type reads as an error.
    if( !id.is_valid() || ( types_.type_of( id ).is_valid() && table_.is_error( types_.type_of( id ) ) ) )
    {
        return std::nullopt;
    }

    switch( ast_.kind( id ) )
    {
    case Node_kind::Float_literal:
    {
        const Literal_id value { ast_.aux( id ) };

        return value.is_valid() ? std::optional<f64>( literal_pool_.floating( value ) ) : std::nullopt;
    }

    case Node_kind::Unary_expr:
    {
        const std::optional<f64> operand = fold_float( ast_.child( id, 0 ) );

        if( !operand )
        {
            return std::nullopt;
        }

        switch( static_cast<Token_kind>( ast_.aux( id ) ) )
        {
        case Token_kind::Minus:
            return -*operand;

        case Token_kind::Plus:
            return operand;

        default:
            return std::nullopt;
        }
    }

    // An integer literal that adopted a float type keeps its value in the integer pool - the lexer
    // decides the pool and the checker never moves it. Without this `f64 a = 1;` at file scope
    // records nothing and is emitted uninitialised. No guard on the recorded type: every caller has
    // already settled that the context is a float.
    case Node_kind::Char_literal:
    case Node_kind::Int_literal:
    {
        const Folded value = fold_integer( id );

        return value.constant ? std::optional<f64>( static_cast<f64>( value.value.magnitude ) ) : std::nullopt;
    }

    // Child 1 is the value, child 0 the annotation. The recursion settles which pool the operand
    // is in. No rounding to the target's width, which would be wrong rather than thorough:
    // check_constant measures this value with fits_float, and pre-rounding an overflow to infinity
    // is exactly the case that check exists to catch.
    case Node_kind::Cast_expr:
        return fold_float( ast_.child( id, 1 ) );

    case Node_kind::Binary_expr:
    {
        const std::optional<f64> left  = fold_float( ast_.child( id, 0 ) );
        const std::optional<f64> right = fold_float( ast_.child( id, 1 ) );

        if( !left || !right )
        {
            return std::nullopt;
        }

        switch( static_cast<Token_kind>( ast_.aux( id ) ) )
        {
        case Token_kind::Plus:
            return *left + *right;

        case Token_kind::Minus:
            return *left - *right;

        case Token_kind::Star:
            return *left * *right;

        case Token_kind::Slash:
            return *right == 0.0 ? std::nullopt : std::optional<f64>( *left / *right );

        default:
            return std::nullopt;
        }
    }

    default:
        return std::nullopt;
    }
}

bool Constant_folder::check_constant( Node_id id, Type_id type )
{
    const bool integer  = table_.is_integer( type );
    const bool floating = table_.is_float( type );
    const bool generic  = table_.is_parameter( type );

    if( !integer && !floating && !generic )
    {
        return true; // bool, pointers, structs: nothing here can overflow
    }

    // These two are about the operator rather than the value, so they apply even when what is
    // being divided or shifted is not itself a constant.
    if( ast_.kind( id ) == Node_kind::Binary_expr )
    {
        const Token_kind op    = static_cast<Token_kind>( ast_.aux( id ) );
        const Node_id    right = ast_.child( id, 1 );

        if( op == Token_kind::Slash || op == Token_kind::Percent )
        {
            const Folded divisor = fold_integer( right );

            // Which pool the divisor lives in is the *node's* question, not the type's: inside a
            // generic it has adopted `T`, which is neither an integer nor a float, and asking
            // Literal_pool for a float it never stored reads an unrelated value out of the wrong
            // vector. A concrete type answers it directly; a parameter is told by the literal.
            const bool by_float = floating || ( generic && ast_.kind( right ) == Node_kind::Float_literal );

            const bool zero = by_float ? ( fold_float( right ).value_or( 1.0 ) == 0.0 )
                                       : ( divisor.constant && !divisor.overflowed && divisor.value.magnitude == 0 );

            if( zero )
            {
                reporter_.error_at( ast_.span( id ), op == Token_kind::Percent ? "remainder by zero" : "division by zero" );
                return false;
            }
        }

        if( op == Token_kind::Less_less || op == Token_kind::Greater_greater )
        {
            const Folded count = fold_integer( right );

            // A parameter has no width of its own, so the count must be in range for the narrowest
            // integer it may turn out to be - the same rule a literal's value is held to, and for
            // the same reason: it is answerable here, so D11 does not have to give anything up.
            const u8 width = generic ? bounds_.narrowest_admissible_width( type ) : table_.get( type ).width;

            if( width != 0 && count.constant && ( count.overflowed || count.value.negative || count.value.magnitude >= width ) )
            {
                reporter_.error_at(
                    ast_.span( right ),
                    "the shift count is out of range",
                    generic ? fmt::format(
                                  "`{}` may be {} bits wide, so the count must be between 0 and {}",
                                  table_.name( type ),
                                  width,
                                  width - 1
                              )
                            : fmt::format(
                                  "`{}` is {} bits wide, so the count must be between 0 and {}",
                                  table_.name( type ),
                                  width,
                                  width - 1
                              )
                );

                return false;
            }
        }
    }

    // Everything below measures a folded *value* against the type, which a parameter does not fix.
    // Nothing is lost: a constant expression of two literals settles on the default type rather
    // than on `T` - only a lone literal is ever typed `T`, and Literals::check_literal ranged it already.
    if( generic )
    {
        return true;
    }

    if( floating )
    {
        const std::optional<f64> value = fold_float( id );

        // An infinity from finite operands is an overflow, and f64 has no range check of its own
        // to catch it.
        if( value && ( !std::isfinite( *value ) || !table_.fits_float( *value, type ) ) )
        {
            reporter_.error_at( ast_.span( id ), fmt::format( "`{}` does not fit in `{}`", *value, table_.name( type ) ) );
            return false;
        }

        return true;
    }

    // The type is passed in: this runs before the node's own is recorded, and the operators that
    // need a width would otherwise fold to nothing exactly where overflow is being checked.
    const Folded value = fold_integer( id, type );

    if( !value.constant )
    {
        return true;
    }

    if( value.overflowed )
    {
        reporter_.error_at( ast_.span( id ), fmt::format( "this constant does not fit in `{}`", table_.name( type ) ) );
        return false;
    }

    if( !table_.fits( value.value.magnitude, value.value.negative, type ) )
    {
        reporter_.error_at(
            ast_.span( id ),
            fmt::format(
                "`{}{}` does not fit in `{}`", value.value.negative ? "-" : "", value.value.magnitude, table_.name( type )
            )
        );

        return false;
    }

    return true;
}

// Recording the error type on a rejected constant is what stops the enclosing operation folding
// through it and reporting the same mistake again.
Type_id Constant_folder::record_constant( Node_id id, Type_id type )
{
    if( !check_constant( id, type ) )
    {
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }

    return types_.record( id, type );
}

void Constant_folder::record_value( Node_id id, Constant_value value )
{
    constants_.emplace( id.v, value );
}

std::optional<Constant_value> Constant_folder::value_of( Node_id id ) const
{
    const auto found = constants_.find( id.v );

    if( found == constants_.end() )
    {
        return std::nullopt;
    }

    return found->second;
}

std::unordered_map<u32, Constant_value> Constant_folder::take_values()
{
    return std::move( constants_ );
}

} // namespace sema
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

// The rule is what the compiler can actually evaluate, and since the constant folder landed that
// is more than a bare literal. C accepts arithmetic constant expressions at file scope too, so the
// emitter can print the expression rather than needing a value computed for it.
TEST_CASE( "type_checker_accepts_a_constant_expression_global", "[sema][types][globals][constants]" )
{
    SECTION( "arithmetic" )
    {
        const Typed p( "i32 limit = 60 * 60;\ni32 main() { return limit; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the operators that fold, and the ones that cannot overflow" )
    {
        for( const char* head :
             { "i32 a = 1 + 2 * 3;",
               "i32 b = ( 1 + 2 ) * 3;",
               "u8  c = 255 & 15;",
               "u32 d = 1 << 4;",
               "i32 e = ~0;",
               "i32 f = -( 3 * 3 );",
               "i32 g = 7 % 3;",
               "f64 h = 1.5 * 2.0;",
               "bool i = 1 < 2;" } )
        {
            const Typed p( std::string( head ) + "\ni32 main() { return 0; }\n" );

            INFO( head << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    // The idiom for a deliberately wrapped constant, now writable where it is most wanted.
    SECTION( "a wrap, which is how an all-ones mask is spelled" )
    {
        const Typed p( "u32 mask = wrap<u32>( 0 - 1 );\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Constant rejection applies at file scope exactly as it does anywhere else.
    SECTION( "and it is still measured against the type" )
    {
        for( const char* head : { "u8 over = 200 + 100;", "i32 wide = 2000000000 * 2;", "i32 bad = 1 / 0;" } )
        {
            const Typed p( std::string( head ) + "\ni32 main() { return 0; }\n" );

            INFO( head << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    // Short-circuit operators emit control flow, which file scope has nowhere to put - so they are
    // outside the rule however constant their operands are.
    SECTION( "but not the short-circuit operators" )
    {
        const Typed p( "bool ready = true && false;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

TEST_CASE( "type_checker_rejects_a_computed_global_initialiser", "[sema][types][globals]" )
{
    SECTION( "a call" )
    {
        const Typed p( "i32 make() { return 1; }\ni32 total = make();\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // This is the one the rule exists for: allowing it is the static initialisation order fiasco.
    SECTION( "another global" )
    {
        const Typed p( "i32 first = 1;\ni32 second = first;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "the address of another global" )
    {
        const Typed p( "i32 first = 1;\ni32* second = &first;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // Not a constant-folding question: a struct literal lowers to a temporary and field
    // assignments, which cannot appear at C file scope.
    SECTION( "a struct literal" )
    {
        const Typed p( "struct Point { i32 x; };\nPoint origin = Point { 0 };\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "and a struct-typed global at all" )
    {
        const Typed p( "struct Point { i32 x; };\nPoint origin;\ni32 main() { return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// §12 decided that arithmetic *wraps* at run time. That leaves the constant case, where the answer
// is known and wrapping it is a wrong answer delivered in silence - so a constant that does not fit
// the type its operation happens in is rejected. The value is folded only to decide whether to
// complain; nothing about what gets emitted changes.
TEST_CASE( "type_checker_rejects_constant_overflow", "[sema][types][constants]" )
{
    SECTION( "unsigned subtraction below zero" )
    {
        const Typed p( "i32 main() { u32 d = 1 - 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`-1` does not fit in `u32`" ) != std::string::npos );
    }

    SECTION( "addition past the top" )
    {
        const Typed p( "i32 main() { u8 d = 200 + 100; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`300` does not fit in `u8`" ) != std::string::npos );
    }

    SECTION( "multiplication past the top" )
    {
        const Typed p( "i32 main() { i32 d = 2000000000 * 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`4000000000` does not fit in `i32`" ) != std::string::npos );
    }

    SECTION( "and below the bottom of a signed type" )
    {
        const Typed p( "i32 main() { i8 d = 0 - 200; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // The value has to be measured at every step, not only at the end: this one fits an i32
    // comfortably, and the addition on the way does not. §6.4 says the operation happens in the
    // common type, so that is the type each step is measured against.
    SECTION( "an intermediate result that does not fit" )
    {
        const Typed p( "i32 main() { i32 d = 2000000000 + 2000000000 - 2000000000; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`4000000000` does not fit in `i32`" ) != std::string::npos );
    }

    SECTION( "a negated constant" )
    {
        const Typed p( "i32 main() { u8 d = -( 1 + 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a float constant that overflows to infinity" )
    {
        const Typed p( "i32 main() { f32 d = 1e30 * 1e30; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit in `f32`" ) != std::string::npos );
    }
}

// Undefined behaviour in C rather than merely a wrong answer, and trivially known here.
TEST_CASE( "type_checker_rejects_a_constant_divide_by_zero", "[sema][types][constants]" )
{
    SECTION( "division" )
    {
        const Typed p( "i32 main() { i32 d = 1 / 0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "division by zero" ) != std::string::npos );
    }

    SECTION( "remainder" )
    {
        const Typed p( "i32 main() { i32 d = 1 % 0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "remainder by zero" ) != std::string::npos );
    }

    SECTION( "a divisor that computes to zero" )
    {
        const Typed p( "i32 main() { i32 d = 1 / ( 3 - 3 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // IEEE would make this an infinity rather than undefined, but v0 has no way to write an
    // infinity deliberately, so a constant zero divisor is a mistake whatever the type. One rule
    // rather than an exception that exists only to admit a value nothing can name.
    SECTION( "float division by zero as well" )
    {
        const Typed p( "i32 main() { f64 d = 1.0 / 0.0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "division by zero" ) != std::string::npos );
    }

    SECTION( "and a runtime divisor is not the checker's business" )
    {
        const Typed p( "i32 main() { i32 z = 0; i32 d = 1 / z; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Shifting by the width or more has no defined result in C. The count is usually a constant, so
// this is usually knowable.
TEST_CASE( "type_checker_rejects_a_constant_shift_past_the_width", "[sema][types][constants]" )
{
    SECTION( "wider than the type" )
    {
        const Typed p( "i32 main() { i32 d = 1 << 40; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "shift" ) != std::string::npos );
    }

    SECTION( "exactly the width" )
    {
        const Typed p( "i32 main() { u32 d = 1 << 32; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a negative count" )
    {
        const Typed p( "i32 main() { i32 d = 1 << -1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "one less than the width is fine" )
    {
        const Typed p( "i32 main() { u32 d = 1 << 31; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // 1 << 31 does not fit a signed i32, which is the overflow rule rather than the shift rule.
    SECTION( "but it must still fit the type" )
    {
        const Typed p( "i32 main() { i32 d = 1 << 31; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a runtime count is not rejected" )
    {
        const Typed p( "i32 main() { u32 n = 40; u32 d = 1 << n; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The divisor and the count are constants even when the value being divided or shifted is not, and
// these are the cases worth catching most: a mistake in an expression that otherwise looks like
// ordinary running code. They reach infer_binary rather than check()'s literal branches, which is a
// separate path through the same rule.
TEST_CASE( "type_checker_rejects_a_constant_divisor_of_a_runtime_value", "[sema][types][constants]" )
{
    SECTION( "division" )
    {
        const Typed p( "i32 main() { i32 f = 3; i32 d = f / 0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "division by zero" ) != std::string::npos );
    }

    SECTION( "remainder" )
    {
        const Typed p( "i32 main() { u32 f = 3; u32 d = f % 0; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "remainder by zero" ) != std::string::npos );
    }

    SECTION( "a shift count past the width" )
    {
        const Typed p( "i32 main() { u32 f = 3; u32 d = f << 40; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "shift count" ) != std::string::npos );
    }

    // The width is the *left* operand's, so a narrow type has a correspondingly small limit.
    SECTION( "measured against the type being shifted, not i32" )
    {
        const Typed p( "i32 main() { u8 f = 3; u8 d = f << 9; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and a count inside the width is fine" )
    {
        const Typed p( "i32 main() { u32 f = 3; u32 d = f << 4; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The rule must not reach past constants, and must not refuse arithmetic that is simply correct.
TEST_CASE( "type_checker_allows_constants_that_fit", "[sema][types][constants]" )
{
    SECTION( "ordinary arithmetic" )
    {
        for( const char* body :
             { "i32 d = 2 + 3;",
               "u8 d = 200 + 55;",
               "i32 d = 1 - 2;",
               "i32 d = 6 / 3;",
               "i32 d = 7 % 3;",
               "u8 d = 255 & 15;",
               "i32 d = ~0;",
               "i32 d = -2147483648;",
               "u64 d = 4294967295 * 2;" } )
        {
            const Typed p( std::string( "i32 main() { " ) + body + " return 0; }" );

            INFO( body << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    // A value that leaves u64 entirely is an overflow of every Keel type, so it is still refused -
    // the folder running out of room is not a reason to stay quiet.
    SECTION( "past what u64 can hold" )
    {
        const Typed p( "i32 main() { u64 d = 18446744073709551615 * 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "nothing involving a variable is folded" )
    {
        for( const char* body :
             { "u8 a = 200; u8 d = a + a;", "i32 a = 2000000000; i32 d = a * 2;", "u32 a = 1; u32 d = a - 2;" } )
        {
            const Typed p( std::string( "i32 main() { " ) + body + " return 0; }" );

            INFO( body << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    // `wrap` infers its operand rather than pushing the target type in, so the fold happens in i32
    // where -1 fits. That is the opt-out for deliberate wrapping, and it falls out of D28 rather
    // than being a special case here.
    SECTION( "wrap is the way to ask for it deliberately" )
    {
        const Typed p( "i32 main() { u32 mask = wrap<u32>( 0 - 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // cast pushes the type in, so the constant is measured against u32 and refused.
    SECTION( "and cast is not" )
    {
        const Typed p( "i32 main() { u32 mask = cast<u32>( 0 - 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "comparisons cannot overflow and are left alone" )
    {
        const Typed p( "i32 main() { if ( 200 + 100 > 0 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The folder was written to *detect* overflow, so it answered "not constant" for every operator
// that cannot overflow. Evaluating a global's initialiser needs it to answer properly, and the
// constant-overflow rule gets wider for free: `~0 + 1` is not checked today because the `~` stops
// the fold before the addition is reached.
TEST_CASE( "type_checker_folds_bitwise_operators", "[sema][types][constants][fold]" )
{
    // Bitwise operators are defined on the representation, so folding one means going to two's
    // complement in the type's width and back. Sign-magnitude has no bit pattern of its own.
    SECTION( "and, or, xor on positive values" )
    {
        for( const char* body :
             { "u8 d = 255 & 15; if ( d != 15 ) { return 1; }",
               "u8 d = 240 | 15; if ( d != 255 ) { return 1; }",
               "u8 d = 255 ^ 15; if ( d != 240 ) { return 1; }" } )
        {
            const Typed p( std::string( "i32 main() { " ) + body + " return 0; }" );

            INFO( body << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    // The round trip a sign bug breaks: ~0 is -1 in every signed width. Asserted on the *value*
    // rather than on cleanliness, because ignoring the sign bit still produces a clean program -
    // just one holding 4294967295.
    SECTION( "complement of zero is negative one" )
    {
        const Typed p( "i32 flipped = ~0;\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::optional<Constant_value> value = p.constant_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE( value.has_value() );
        REQUIRE( value->negative );
        REQUIRE( value->magnitude == 1 );
    }

    // The same pattern in an unsigned type is the maximum, not minus one - which is the whole
    // reason from_bits needs the signedness and not just the width.
    SECTION( "and the maximum in an unsigned one" )
    {
        const Typed p( "u32 all_ones = wrap<u32>( ~0 );\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const std::optional<Constant_value> value = p.constant_of( p.nth( Node_kind::Var_decl, 0 ) );

        REQUIRE( value.has_value() );
        REQUIRE_FALSE( value->negative );
        REQUIRE( value->magnitude == 4294967295ull );
    }

    // ~0 does not fit an unsigned type as -1, but as a bit pattern it is the maximum. The width is
    // what decides, which is why the fold needs the type rather than just the value.
    SECTION( "complement in an unsigned type is the maximum" )
    {
        const Typed p( "i32 main() { u8 d = ~0; return 0; }" );

        INFO( p.rendered() );

        // -1 does not fit u8, so this is refused - and it is the *fold* that knows, not the parser.
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and on negative values, through the sign conversion" )
    {
        const Typed p( "i32 main() { i32 d = -1 & 255; if ( d != 255 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The point of folding these at all: a bitwise operand no longer stops the fold, so the value
    // reaches the enclosing operation.
    SECTION( "a bitwise operand no longer stops the fold" )
    {
        const Typed p( "i32 main() { u8 d = 255 & 255; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Nested here is still refused for an unrelated reason: check() pushes an expectation into a
    // binary only when both operands are literal expressions, and a Binary_expr is not one - so
    // `( 255 & 255 )` settles on i32 before the addition. That is the inferring-where-checking-
    // belonged family again, and widening Literals::is_literal_expression would fix it.
    SECTION( "though a nested one is still refused, for a different reason" )
    {
        const Typed p( "i32 main() { u8 d = ( 255 & 255 ) + 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

TEST_CASE( "type_checker_folds_comparisons", "[sema][types][constants][fold]" )
{
    SECTION( "producing a bool" )
    {
        for( const char* body :
             { "bool d = 1 < 2; if ( !d ) { return 1; }",
               "bool d = 2 < 1; if ( d ) { return 1; }",
               "bool d = 2 == 2; if ( !d ) { return 1; }",
               "bool d = 2 != 2; if ( d ) { return 1; }" } )
        {
            const Typed p( std::string( "i32 main() { " ) + body + " return 0; }" );

            INFO( body << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    // Negative sorts below positive whatever the magnitudes, which sign-magnitude does not give
    // for free.
    SECTION( "with a negative operand" )
    {
        const Typed p( "i32 main() { bool d = -5 < 1; if ( !d ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A cast reaching the folder is either a widening `cast` or a `wrap` - narrowing `cast` is refused
// by the checker until there is something to trap with - so folding one is reducing modulo the
// target's width, which is the same machinery the bitwise operators need.
TEST_CASE( "type_checker_folds_casts", "[sema][types][constants][fold]" )
{
    SECTION( "a widening cast keeps the value" )
    {
        const Typed p( "i32 main() { i64 d = cast<i64>( 7 ); if ( d != 7 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The all-ones idiom, and the one that exercises the sign conversion in both directions.
    SECTION( "a wrap reduces modulo the width" )
    {
        const Typed p( "i32 main() { u32 d = wrap<u32>( 0 - 1 ); if ( d != 4294967295 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a narrowing wrap keeps the low bits" )
    {
        const Typed p( "i32 main() { u8 d = wrap<u8>( 300 ); if ( d != 44 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Which pool a global's literals live in, not the type it was given, is what decides whether its
// initialiser folds: an integer literal that adopted a float type keeps its value in the integer
// pool, and a cast keeps it one level further down again.
TEST_CASE( "type_checker_folds_every_float_global", "[sema][types][constants][fold]" )
{
    // Through the root rather than by walking the node array, so a case that later grows a struct
    // or a method still addresses the declaration it names.
    const auto value_of = []( const Typed& p, std::size_t index ) { return p.constant_of( p.child( p.ast().root(), index ) ); };

    SECTION( "an integer literal that adopted the type" )
    {
        const Typed p( "f64 a = 1;\nf32 b = 2;\nf64 n = -1;\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        for( std::size_t i = 0; i < 3; ++i )
        {
            INFO( "global " << i );
            REQUIRE( value_of( p, i ).has_value() );
            REQUIRE( value_of( p, i )->kind == Constant_value::Kind::Float );
        }

        REQUIRE( value_of( p, 0 )->floating == 1.0 );
        REQUIRE( value_of( p, 1 )->floating == 2.0 );
        REQUIRE( value_of( p, 2 )->floating == -1.0 );
    }

    SECTION( "a cast" )
    {
        const Typed p( "f64 j = cast<f64>( 2.5 );\nf32 f = cast<f32>( 1 );\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        REQUIRE( value_of( p, 0 ).has_value() );
        REQUIRE( value_of( p, 0 )->floating == 2.5 );
        REQUIRE( value_of( p, 1 ).has_value() );
        REQUIRE( value_of( p, 1 )->floating == 1.0 );
    }

    SECTION( "a cast inside an expression" )
    {
        const Typed p( "f64 h = cast<f64>( 1 ) + 1.0;\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        REQUIRE( value_of( p, 0 ).has_value() );
        REQUIRE( value_of( p, 0 )->floating == 2.0 );
    }

    // The divisor is an integer literal that adopted `f64`, so the float pool is the only one the
    // operator check asks about it - and a divisor it cannot value is a divisor it cannot refuse.
    SECTION( "a zero divisor the integer pool holds" )
    {
        const Typed p( "f64 x = 1.0 / 0;\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "division by zero" ) != std::string::npos );
    }
}

// Every accepted file-scope initialiser must fold, because the rule already requires it to be a
// constant expression. A failure here is an internal error rather than a user one, which is a
// stronger invariant than the expression printer it replaces.
TEST_CASE( "type_checker_records_a_value_for_every_global", "[sema][types][constants][fold]" )
{
    const Typed p( "i32  counter = 1;\n"
                   "f64  ratio = 1.5;\n"
                   "bool ready = true;\n"
                   "i32  below = -1;\n"
                   "i32  computed = 60 * 60;\n"
                   "u8   masked = 255 & 15;\n"
                   "u32  shifted = 1 << 4;\n"
                   "i32  flipped = ~0;\n"
                   "bool compared = 1 < 2;\n"
                   "u32  all_ones = wrap<u32>( 0 - 1 );\n"
                   "i32* nothing = nullptr;\n"
                   "i32  blank;\n"
                   "i32 main() { return 0; }\n" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );

    // One per global with an initialiser; `blank` has none and needs no value.
    std::size_t recorded = 0;

    for( u32 i = 0; i < 12; ++i )
    {
        const Node_id global = p.nth( Node_kind::Var_decl, i );

        if( !global.is_valid() )
        {
            break;
        }

        const Node_id init = p.child( global, 1 );

        if( init.is_valid() )
        {
            INFO( "global " << i );
            REQUIRE( p.constant_of( global ).has_value() );
            recorded += 1;
        }
    }

    REQUIRE( recorded == 11 );
}

// A literal in a generic body reopened checks that were unreachable while `T` took none: the
// operator-level ones are answerable without knowing `T` and must still run, and routing the
// parameter path around record_constant is how a generic body became the one place they did not.
TEST_CASE( "type_checker_checks_constants_in_a_generic_body", "[sema][generic][bound]" )
{
    SECTION( "division and remainder by a literal zero" )
    {
        const Typed divide( "T f<T>( T a ) where T : Integral { return a / 0; }\ni32 main() { return 0; }" );
        const Typed modulo( "T f<T>( T a ) where T : Integral { return a % 0; }\ni32 main() { return 0; }" );

        INFO( divide.rendered() << modulo.rendered() );
        REQUIRE( divide.rendered().find( "division by zero" ) != std::string::npos );
        REQUIRE( modulo.rendered().find( "remainder by zero" ) != std::string::npos );
    }

    SECTION( "a fractional zero divisor, which lives in the other pool" )
    {
        // Reached by the literal's node kind rather than by the type: inside a generic the divisor
        // has adopted `T`, which is neither an integer nor a float, so the type cannot say which
        // pool holds it.
        const Typed p( "T f<T>( T a ) where T : Floating { return a / 0.0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "division by zero" ) != std::string::npos );
    }

    SECTION( "a non-zero divisor is left alone" )
    {
        const Typed p( "T f<T>( T a ) where T : Integral { return a / 2; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a shift count is held to the narrowest width the bound admits" )
    {
        // `Integral` admits `i8`, so the count must be in range for eight bits. Same rule as the
        // literal's value, and answerable for the same reason.
        const Typed fits( "T f<T>( T a ) where T : Integral { return a << 7; }\ni32 main() { return 0; }" );
        const Typed over( "T f<T>( T a ) where T : Integral { return a << 8; }\ni32 main() { return 0; }" );

        INFO( fits.rendered() << over.rendered() );
        REQUIRE( fits.clean() );
        REQUIRE( over.errors() == 1 );
        REQUIRE( over.rendered().find( "the shift count is out of range" ) != std::string::npos );
        REQUIRE( over.rendered().find( "`T` may be 8 bits wide" ) != std::string::npos );
    }
}

} // namespace keel
#endif
