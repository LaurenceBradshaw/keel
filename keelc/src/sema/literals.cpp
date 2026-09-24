#include "sema/literals.h"
#include <fmt/format.h>
#include <cassert>

namespace keel
{
namespace sema
{

bool Literals::is_literal_expression( Node_id id ) const
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
    {
        // `-5` and `-1.5` are negations of literals, and check_literal pushes the expectation
        // through the minus.
        if( static_cast<Token_kind>( ast_.aux( id ) ) != Token_kind::Minus )
        {
            return false;
        }

        const Node_kind operand = ast_.kind( ast_.child( id, 0 ) );

        return operand == Node_kind::Int_literal || operand == Node_kind::Float_literal;
    }

    default:
        return false;
    }
}

Type_id Literals::infer_literal( Node_id id )
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Int_literal:
        return types_.record( id, table_.default_integer() );

    case Node_kind::Float_literal:
        return types_.record( id, table_.default_float() );

    case Node_kind::Bool_literal:
        return types_.record( id, table_.builtin( Type_kind::Bool ) );

    case Node_kind::Char_literal:
        return types_.record( id, table_.integer( 8, false ) ); // D20: a code point, so a u8

    case Node_kind::Null_literal:
        reporter_.error_at( ast_.span( id ), "cannot infer type of `nullptr`" );
        return types_.record( id, table_.builtin( Type_kind::Error ) );

    default:
        assert( false );
        return types_.record( id, table_.builtin( Type_kind::Error ) );
    }
}

Type_id Literals::check_literal( Node_id id, Type_id expected )
{
    // `-2147483648` parses as a negation of 2147483648, which does not fit an i32 on its own. So
    // the expectation is pushed through the minus and the range check is told the value is
    // negative - exactly the asymmetry Type_table::fits() carries.
    Node_id literal  = id;
    bool    negative = false;

    if( ast_.kind( id ) == Node_kind::Unary_expr && static_cast<Token_kind>( ast_.aux( id ) ) == Token_kind::Minus &&
        is_literal_expression( ast_.child( id, 0 ) ) )
    {
        literal = ast_.child( id, 0 );

        // Only integers have an asymmetric range. A float's negation cannot take it out of range.
        negative = ast_.kind( literal ) == Node_kind::Int_literal;
    }

    // D26 with a type parameter as the context. What a bound admits is a *set* of types, so the
    // question is not "does the value fit `T`" - there is no such type yet - but "does it fit every
    // type `T` may turn out to be". Answering it here keeps D11 whole: nothing is deferred to an
    // instantiation, and no later call site can break a body that compiled.
    if( table_.is_parameter( expected ) )
    {
        // An unbounded parameter has no entry at all rather than an empty one, which bounds_for
        // answers as the empty set: it promises nothing, and fails the test below.
        const Bound_set bounds = bounds_.bounds_for( expected );

        const bool floating_literal = ast_.kind( literal ) == Node_kind::Float_literal;

        // Neither carries a Literal_id - a bool's value is in aux and `nullptr` records nothing - so
        // the range check below reads an invalid id, finds nothing to reject, and would let them
        // adopt `T`. They are refused on kind because no bound admits either: nothing in the set
        // makes a `T` a bool, and `Equatable` cannot tell a pointer from an `enum`.
        if( ast_.kind( literal ) == Node_kind::Bool_literal || ast_.kind( literal ) == Node_kind::Null_literal )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format(
                    "{} cannot be a `{}`",
                    ast_.kind( literal ) == Node_kind::Bool_literal ? "a `bool` literal" : "a null literal",
                    table_.name( expected )
                ),
                "no bound promises that, so the parameter has to be written out as a type"
            );

            return expected;
        }

        // `Numeric` is the promise that says `T` is a number, and it is what licenses treating the
        // admissible set as numeric at all - every numeric type satisfies `Equatable` too, but so
        // do `bool`, every `enum` and every pointer, and a literal is none of those.
        if( !contains( bounds, Bound::Numeric ) )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format( "a literal cannot be a `{}`", table_.name( expected ) ),
                fmt::format( "write `where {} : Numeric` to promise it is a number", table_.name( expected ) )
            );

            return expected;
        }

        // An integer literal needs nothing more: every numeric type represents one, which is why
        // `f64 x = 1;` needs no decimal point. A *float* literal does, because a `Numeric` `T` may
        // turn out to be an integer and a fractional value is not one.
        if( floating_literal && !contains( bounds, Bound::Floating ) )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format( "a fractional literal cannot be a `{}`", table_.name( expected ) ),
                fmt::format(
                    "`{}` may be an integer; write `where {} : Floating`", table_.name( expected ), table_.name( expected )
                )
            );

            return expected;
        }

        if( const Type_id rejects = bounds_.type_the_literal_overflows( literal, negative, bounds ); rejects.is_valid() )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format(
                    "`{}{}` does not fit every type `{}` may be",
                    negative ? "-" : "",
                    floating_literal ? fmt::format( "{}", literals_.floating( Literal_id { ast_.aux( literal ) } ) )
                                     : fmt::format( "{}", literals_.integer( Literal_id { ast_.aux( literal ) } ) ),
                    table_.name( expected )
                ),
                fmt::format( "the bounds admit `{}`, which cannot hold it", table_.name( rejects ) )
            );

            return expected;
        }

        // The adoption, which the switch below would otherwise refuse: its arms ask whether
        // `expected` is an integer or a float, and a parameter is neither.
        types_.record( literal, expected );
        return types_.record( id, expected );
    }

    switch( ast_.kind( literal ) )
    {
    case Node_kind::Bool_literal:
        if( expected != table_.builtin( Type_kind::Bool ) )
        {
            reporter_.error_at( ast_.span( id ), fmt::format( "expected `{}`, but got `bool`", table_.name( expected ) ) );
            return expected;
        }

        break;

    case Node_kind::Float_literal:
    {
        if( !table_.is_float( expected ) )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format( "expected `{}`, but got a floating-point literal", table_.name( expected ) ),
                table_.is_integer( expected ) ? "a fractional value cannot be an integer" : std::string {}
            );

            return expected;
        }

        const Literal_id value = Literal_id { ast_.aux( literal ) };

        // A literal the lexer could not scan has no value recorded. It reported there.
        if( value.is_valid() && !table_.fits_float( literals_.floating( value ), expected ) )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format(
                    "`{}{}` does not fit in `{}`",
                    literal == id ? "" : "-", // the sign lives in the Unary_expr, not in the value
                    literals_.floating( value ),
                    table_.name( expected )
                )
            );

            return expected;
        }

        break;
    }

    case Node_kind::Char_literal: // a code point is an integer value, measured the same way
    case Node_kind::Int_literal:
    {
        if( !table_.is_integer( expected ) && !table_.is_float( expected ) )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format(
                    "expected `{}`, but got {}",
                    table_.name( expected ),
                    ast_.kind( literal ) == Node_kind::Char_literal ? "a character literal" : "an integer literal"
                )
            );

            return expected;
        }

        const Literal_id value = Literal_id { ast_.aux( literal ) };

        // A literal the lexer could not scan - one that overflowed a u64, say - has no value
        // recorded. It was reported there; saying so again here helps nobody.
        if( value.is_valid() && !table_.fits( literals_.integer( value ), negative, expected ) )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format(
                    "`{}{}` does not fit in `{}`", negative ? "-" : "", literals_.integer( value ), table_.name( expected )
                )
            );

            return expected;
        }

        break;
    }
    case Node_kind::Null_literal:
        if( !table_.is_pointer( expected ) )
        {
            reporter_.error_at(
                ast_.span( id ),
                fmt::format( "expected `{}`, but got a null literal", table_.name( expected ) ),
                "a null literal can only be assigned to a pointer"
            );

            return expected;
        }

        break;

    default:
        return expected; // not a literal after all
    }

    // The literal adopts the expected type - the whole point of checking rather than inferring,
    // and what lets `u32 x = 42;` need no suffix. The negation, when there is one, takes it too.
    types_.record( literal, expected );
    return types_.record( id, expected );
}

// Its default first, because that is D26's answer and the one an author predicts; a value too
// large for that takes whichever 64-bit type holds it. Which one is picked cannot change the
// answer - every type holding the value exactly compares the same - so this decides only where
// the comparison happens.
//
// Integer literals only. A float literal that overflows an `f32` is a precision question rather
// than this one, and it keeps the error it has today.
Type_id Literals::standalone_literal_type( Node_id id, Type_id known ) const
{
    Node_id literal  = id;
    bool    negative = false;

    if( ast_.kind( id ) == Node_kind::Unary_expr && static_cast<Token_kind>( ast_.aux( id ) ) == Token_kind::Minus &&
        is_literal_expression( ast_.child( id, 0 ) ) )
    {
        literal  = ast_.child( id, 0 );
        negative = ast_.kind( literal ) == Node_kind::Int_literal;
    }

    if( ast_.kind( literal ) != Node_kind::Int_literal && ast_.kind( literal ) != Node_kind::Char_literal )
    {
        return Type_id {};
    }

    const Literal_id value { ast_.aux( literal ) };

    // Nothing the lexer could not scan, and nothing against a type with no range to be outside
    // of - a parameter's admissible set is slice 1e's question and is answered by check_literal.
    if( !value.is_valid() || ( !table_.is_integer( known ) && !table_.is_float( known ) ) )
    {
        return Type_id {};
    }

    const u64 magnitude = literals_.integer( value );

    if( table_.fits( magnitude, negative, known ) )
    {
        return Type_id {};
    }

    for( const Type_id candidate : { table_.default_integer(), table_.integer( 64, true ), table_.integer( 64, false ) } )
    {
        if( table_.fits( magnitude, negative, candidate ) )
        {
            return candidate;
        }
    }

    return Type_id {};
}

// The other half of the rule above: a comparison against a constant the operand beside it cannot
// hold is legal, and its answer does not depend on that operand at all. D41 is why it compiles;
// this is why it is still worth saying. Deliberately a warning - the code is well defined and
// means something, it just does not mean what it looks like.
void Literals::warn_if_constant_comparison( Node_id id, Token_kind op, Type_id lhs_type, Type_id rhs_type )
{
    const Folded folded[2] = {
        constant_folder_.fold_integer( ast_.child( id, 0 ) ), constant_folder_.fold_integer( ast_.child( id, 1 ) )
    };

    // Either side may be the constant, and when both are, either may be the one out of range -
    // `300 > cast<u8>( 200 )` is as settled as `b < -1` is. So both are asked, rather than one
    // being picked and the other trusted.
    for( const bool constant_on_the_left : { true, false } )
    {
        const Folded  value = constant_on_the_left ? folded[0] : folded[1];
        const Type_id other = constant_on_the_left ? rhs_type : lhs_type;

        if( !value.constant || value.overflowed || table_.fits( value.value.magnitude, value.value.negative, other ) )
        {
            continue;
        }

        // A range is contiguous, so a value outside it is above all of it or below all of it, and
        // for an integer the sign says which: a negative that does not fit is below the minimum,
        // and a non-negative one is above the maximum. That settles the ordering, and the operator
        // does the rest.
        const bool left_is_smaller = constant_on_the_left == value.value.negative;

        const bool answer =
            left_is_smaller ? ( op == Token_kind::Less || op == Token_kind::Less_equal || op == Token_kind::Bang_equal )
                            : ( op == Token_kind::Greater || op == Token_kind::Greater_equal || op == Token_kind::Bang_equal );

        reporter_.warn_at(
            ast_.span( id ),
            fmt::format( "this comparison is always {}", answer ? "true" : "false" ),
            fmt::format(
                "`{}{}` is outside the range of `{}`",
                value.value.negative ? "-" : "",
                value.value.magnitude,
                table_.name( other )
            )
        );

        return;
    }
}

} // namespace sema
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

// The whole point of bidirectional checking (L5): a literal has a value, not a type, until
// something tells it what to be. Neither of these needs a suffix.
TEST_CASE( "type_checker_takes_a_literal_type_from_context", "[sema][types]" )
{
    SECTION( "an annotation supplies it" )
    {
        const Typed p( "i32 main() { u32 x = 42; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u32" );
    }

    SECTION( "so does a wider one" )
    {
        const Typed p( "i32 main() { u64 x = 42; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "u64" );
    }

    // Pins the pending D-entry on the default type of an unsuffixed literal.
    SECTION( "with no context it falls back to i32" )
    {
        const Typed p( "i32 main() { auto x = 42; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Int_literal, 0 ) ) == "i32" );
    }

    SECTION( "a float literal likewise" )
    {
        const Typed p( "i32 main() { f32 x = 1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Float_literal, 0 ) ) == "f32" );
    }
}

TEST_CASE( "type_checker_rejects_a_literal_that_does_not_fit", "[sema][types]" )
{
    static const char* sources[] = {
        "i32 main() { u8 x = 300; return 0; }",
        "i32 main() { i8 x = 200; return 0; }",
        "i32 main() { u32 x = 4294967296; return 0; }",
        "i32 main() { u8 x = -1; return 0; }", // negative never fits an unsigned type
    };

    for( const char* source : sources )
    {
        const Typed p( source );

        INFO( "source: " << source << "\n" << p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }
}

// D20: a code point is a u8, so it goes through exactly the machinery integer literals do.
TEST_CASE( "type_checker_types_a_character_literal_as_an_integer", "[sema][types]" )
{
    SECTION( "it adopts the annotated type, like any literal" )
    {
        const Typed p( "i32 main() { u8 c = 'A'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Char_literal, 0 ) ) == "u8" );
    }

    SECTION( "a wider integer works too" )
    {
        const Typed p( "i32 main() { u32 c = 'z'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Char_literal, 0 ) ) == "u32" );
    }

    SECTION( "with no context it falls back to u8" )
    {
        const Typed p( "i32 main() { auto c = 'A'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Char_literal, 0 ) ) == "u8" );
    }

    SECTION( "an escape past 127 still fits a u8" )
    {
        const Typed p( "i32 main() { u8 c = '\\xFF'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "but a bool is not an integer" )
    {
        const Typed p( "i32 main() { bool c = 'a'; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "character literal" ) != std::string::npos );
    }

    SECTION( "and arithmetic on one is arithmetic on a u8" )
    {
        const Typed p( "u8 f( u8 c ) { return c; }\ni32 main() { u8 x = f( 'a' ); return 0; }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A string has no type until String exists (M7), so it must say so rather than vanish.
TEST_CASE( "type_checker_rejects_a_string_literal", "[sema][types]" )
{
    const Typed p( "i32 main() { u8 s = \"hello\"; return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "string literals are not supported yet" ) != std::string::npos );
}

// A literal takes its type from the operand beside it. Without this, `u32 bits; bits != 0` compares
// a u32 with an i32 - which §6.4 rejects, and which no suffix exists to write around (D13). Almost
// no unsigned code would compile.
TEST_CASE( "type_checker_lets_a_literal_adopt_the_other_operand", "[sema][types]" )
{
    SECTION( "comparison against an unsigned variable" )
    {
        const Typed p( "i32 main() { u32 bits = 1; while( bits != 0 ) { bits = bits >> 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "on either side" )
    {
        const Typed p( "i32 main() { u8 b = 1; if( 0 < b ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and in arithmetic, not just comparison" )
    {
        const Typed p( "i32 main() { u64 n = 1; n = n + 1; n = n & 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Adopting is not the same as ignoring: in arithmetic the value still has to fit what it
    // adopted, because there is a result and it has to land somewhere.
    SECTION( "a literal that does not fit the adopted type is still rejected in arithmetic" )
    {
        const Typed p( "i32 main() { u8 b = 1; b = b + 300; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );

        // The literal itself, not the assignment downstream of it. Letting it take a type of its
        // own here would report `expected u8, but got i32` and point at the wrong thing.
        REQUIRE( p.rendered().find( "`300` does not fit in `u8`" ) != std::string::npos );
    }

    // D41: a comparison has an answer whether or not the literal fits, so it is accepted and the
    // answer is what gets reported. The value is outside the range, so nothing about the operand
    // can change it.
    SECTION( "but in a comparison it is accepted and the constant answer is warned about" )
    {
        static const char* sources[] = {
            "i32 main() { u8 b = 1; if( b == 300 ) { return 1; } return 0; }",
            "i32 main() { u32 n = 1; if( n == -1 ) { return 1; } return 0; }",
            "i32 main() { u64 n = 1; if( n < 0 - 1 ) { return 1; } return 0; }",
        };

        for( const char* source : sources )
        {
            const Typed p( source );

            INFO( "source: " << source << "\n" << p.rendered() );
            REQUIRE( p.clean() );
            REQUIRE( p.rendered().find( "this comparison is always false" ) != std::string::npos );
        }
    }

    // Both sides constant, and it is the *left* one that is out of range. Asking only one side
    // would answer this by looking at the wrong operand and finding nothing wrong with it.
    SECTION( "and the constant may be either operand" )
    {
        const Typed p( "i32 main() { if( 300 > cast<u8>( 200 ) ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.rendered().find( "this comparison is always true" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`300` is outside the range of `u8`" ) != std::string::npos );
    }

    SECTION( "and which way the constant answer falls is reported" )
    {
        const Typed p( "i32 main() { u8 b = 1; if( b < 300 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.rendered().find( "this comparison is always true" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`300` is outside the range of `u8`" ) != std::string::npos );
    }

    // Two literals have nothing to adopt from, so both take their defaults.
    SECTION( "two literals still take the default type" )
    {
        const Typed p( "i32 main() { if( 1 < 2 ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // D41: the pair that used to be the error here. Adoption is still what the section is about -
    // neither operand is a literal, so nothing adopts and the comparison stands on its own.
    SECTION( "and two real types of mixed signedness now compare" )
    {
        const Typed p( "i32 main() { i32 a = 1; u32 b = 2; if( a < b ) { return 1; } return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The same family as adopting from the operand beside it: a literal has a value, not a type, and
// every path that can supply one has to. These two were found by writing a program, not by
// inspecting the checker.
TEST_CASE( "type_checker_pushes_the_expected_type_through_to_literals", "[sema][types]" )
{
    SECTION( "a negated float literal adopts, like a negated integer one" )
    {
        const Typed p( "i32 main() { f32 a = -1.5; f64 b = -1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Float_literal, 0 ) ) == "f32" );
    }

    // Both operands are literals, so there is nothing beside them to adopt from and the context
    // is all there is.
    SECTION( "an operation between two literals takes the context's type" )
    {
        const Typed p( "i32 main() { u8 x = 'a' + 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "u8" );
    }

    SECTION( "including when the context is a float" )
    {
        const Typed p( "i32 main() { f32 x = 1 + 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Adopting is not ignoring: each operand still has to fit what it adopted.
    SECTION( "an operand that does not fit the adopted type is rejected" )
    {
        const Typed p( "i32 main() { u8 x = 'a' + 300; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // A comparison yields bool whatever it is handed, so pushing the expectation into it would be
    // nonsense - the operands must still meet each other, not the context.
    SECTION( "a comparison does not take the context's type" )
    {
        const Typed p( "i32 main() { bool b = 1 < 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a non-literal operand still forces a real match" )
    {
        const Typed p( "i32 main() { i32 a = 1; u8 x = a + 1; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// The same rule integer literals get: a value is measured against the type it is being given.
TEST_CASE( "type_checker_range_checks_float_literals", "[sema][types]" )
{
    SECTION( "a value beyond f32's range is rejected" )
    {
        const Typed p( "i32 main() { f32 x = 1e40; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit in `f32`" ) != std::string::npos );
    }

    // The sign lives in the Unary_expr, not in the recorded value, so the message has to put it
    // back or it names a number the author did not write.
    SECTION( "and the message keeps the sign" )
    {
        const Typed p( "i32 main() { f32 x = -1e40; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`-1e+40`" ) != std::string::npos );
    }

    SECTION( "a value at the edge is accepted" )
    {
        const Typed p( "i32 main() { f32 x = 3.4e38; f32 y = -3.4e38; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an inexact value is accepted - range is not representability" )
    {
        const Typed p( "i32 main() { f32 x = 0.1; f32 y = 1e-50; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "f64 takes anything the lexer let through" )
    {
        const Typed p( "i32 main() { f64 x = 1e300; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Reported by the lexer, which is the only place the digits still exist. Sema records no value
    // for it and must not report a second time.
    SECTION( "a value beyond f64 reports exactly once, from the lexer" )
    {
        const Typed p( "i32 main() { f64 x = 1e400; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 0 );
        REQUIRE( p.rendered().find( "out of range" ) != std::string::npos );
    }

    // This regressed when fits_float replaced the is_float() guard: every float-into-integer
    // assignment silently compiled.
    SECTION( "a float literal still cannot be given to an integer" )
    {
        const Typed p( "i32 main() { i32 x = 1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "floating-point literal" ) != std::string::npos );
        REQUIRE( p.rendered().find( "cannot be an integer" ) != std::string::npos );
    }

    SECTION( "nor to a bool" )
    {
        const Typed p( "i32 main() { bool b = 1.5; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// D26: a literal whose type comes from context, exactly like an integer literal.
TEST_CASE( "type_checker_types_nullptr", "[sema][types]" )
{
    SECTION( "it adopts the annotated pointer type" )
    {
        const Typed p( "i32 main() { i32* q = nullptr; u8* r = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Null_literal, 0 ) ) == "i32*" );
        REQUIRE( p.type_name( p.nth( Node_kind::Null_literal, 1 ) ) == "u8*" );
    }

    SECTION( "with no context there is nothing to adopt" )
    {
        const Typed p( "i32 main() { auto q = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "and it is not a value of any other type" )
    {
        for( const char* tail : { "i32 v = nullptr;", "bool b = nullptr;", "f64 d = nullptr;" } )
        {
            const Typed p( std::string( "i32 main() { " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    SECTION( "it can be passed to a pointer parameter" )
    {
        const Typed p( "i32 take( i32* q ) { return 0; }\ni32 main() { return take( nullptr ); }\n" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The other half of absorbing: a literal that genuinely has nothing to adopt from still has to say
// so, and one that is simply out of range must not be waved through. Absorbing too eagerly would
// silence both.
TEST_CASE( "type_checker_still_reports_a_literal_with_no_context", "[sema][types]" )
{
    SECTION( "auto has nothing to give a null literal" )
    {
        const Typed p( "i32 main() { auto p = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot infer type of `nullptr`" ) != std::string::npos );
    }

    SECTION( "a null literal against a real type is still wrong" )
    {
        const Typed p( "i32 main() { i32 v = nullptr; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "range checking survives" )
    {
        const Typed p( "i32 main() { u8 x = 300; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not fit" ) != std::string::npos );
    }

    SECTION( "a literal target is still not assignable" )
    {
        const Typed p( "i32 main() { 1 = 2; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot assign to this expression" ) != std::string::npos );
    }

    // The reason infer_binary pushes the expectation at all: without it these compare a u32 or a u8
    // against an i32, which §6.4 rejects and no suffix can write around.
    SECTION( "and a literal beside a good operand still adopts from it" )
    {
        const Typed a( "i32 main() { u32 bits = 3; if ( bits != 0 ) { return 1; } return 0; }" );
        const Typed b( "i32 main() { u8 i = 97 + 1; return 0; }" );

        INFO( a.rendered() << b.rendered() );
        REQUIRE( a.clean() );
        REQUIRE( b.clean() );
    }
}

TEST_CASE( "type_checker_adopts_a_literal_into_a_type_parameter", "[sema][generic][bound]" )
{
    const auto body = []( std::string_view clause, std::string_view expression )
    { return fmt::format( "T f<T>( T a ) where T : {} {{ return {}; }}\ni32 main() {{ return 0; }}", clause, expression ); };

    SECTION( "an integer literal adopts any numeric parameter" )
    {
        // Including a floating one: every numeric type represents an integer, which is why
        // `f64 x = 1;` needs no decimal point and why the generic case should not differ.
        const Typed numeric( body( "Numeric", "a + 1" ) );
        const Typed integral( body( "Integral", "a + 1" ) );
        const Typed floating( body( "Floating", "a + 1" ) );

        INFO( numeric.rendered() << integral.rendered() << floating.rendered() );
        REQUIRE( numeric.clean() );
        REQUIRE( integral.clean() );
        REQUIRE( floating.clean() );
    }

    SECTION( "a fractional literal needs `Floating` specifically" )
    {
        // `Numeric` is not enough: the parameter may turn out to be an integer, and `1.5` is not
        // one. The asymmetry with the case above is the whole rule.
        const Typed floating( body( "Floating", "a + 1.5" ) );
        const Typed numeric( body( "Numeric", "a + 1.5" ) );
        const Typed integral( body( "Integral", "a + 1.5" ) );

        INFO( floating.rendered() << numeric.rendered() << integral.rendered() );
        REQUIRE( floating.clean() );
        REQUIRE( numeric.errors() == 1 );
        REQUIRE( numeric.rendered().find( "a fractional literal cannot be a `T`" ) != std::string::npos );
        REQUIRE( integral.errors() == 1 );
    }

    SECTION( "a parameter with no numeric bound takes no literal at all" )
    {
        // `Equatable` is the trap: every numeric type satisfies it, but so do `bool`, every `enum`
        // and every pointer, and a literal is none of those. `Numeric` is the promise that says the
        // parameter is a number, and it is what licenses reading the admissible set as numeric.
        const Typed equatable( body( "Equatable", "a" ) + "\nbool g<T>( T a ) where T : Equatable { return a == 0; }" );
        const Typed unbounded( "void f<T>( T a ) { T x = 1; }\ni32 main() { return 0; }" );

        INFO( equatable.rendered() << unbounded.rendered() );
        REQUIRE( equatable.rendered().find( "a literal cannot be a `T`" ) != std::string::npos );
        REQUIRE( equatable.rendered().find( "where T : Numeric" ) != std::string::npos );

        // An unbounded parameter has no entry in the bound table at all, rather than an empty one.
        // Reading that as a broken invariant rather than as "promises nothing" aborted the compiler.
        REQUIRE( unbounded.errors() == 1 );
        REQUIRE( unbounded.rendered().find( "a literal cannot be a `T`" ) != std::string::npos );
    }

    SECTION( "`bool` and `null` never adopt" )
    {
        const Typed boolean( body( "Numeric", "a" ) + "\nvoid g<T>( T a ) where T : Numeric { T x = true; }" );
        const Typed pointer( body( "Numeric", "a" ) + "\nvoid g<T>( T a ) where T : Equatable { T x = nullptr; }" );

        INFO( boolean.rendered() << pointer.rendered() );
        REQUIRE_FALSE( boolean.clean() );
        REQUIRE_FALSE( pointer.clean() );
    }

    SECTION( "the value has to fit every type the bound admits" )
    {
        // `Integral` admits all eight integers, so the window is their intersection: 0..127. That is
        // tighter than it looks and it is the price of D11 - the alternative is checking against the
        // instantiation set, where a call site added later breaks a body that compiled yesterday.
        const Typed zero( body( "Integral", "a + 0" ) );
        const Typed edge( body( "Integral", "a + 127" ) );
        const Typed over( body( "Integral", "a + 128" ) );
        const Typed under( body( "Integral", "a + -1" ) );

        INFO( zero.rendered() << edge.rendered() << over.rendered() << under.rendered() );
        REQUIRE( zero.clean() );
        REQUIRE( edge.clean() );

        // `i8` cannot hold 128, and `u8` cannot hold -1. Both are admissible, so both reject.
        REQUIRE( over.errors() == 1 );
        REQUIRE( over.rendered().find( "does not fit every type `T` may be" ) != std::string::npos );
        REQUIRE( over.rendered().find( "`i8`" ) != std::string::npos );
        REQUIRE( under.errors() == 1 );
        REQUIRE( under.rendered().find( "`u8`" ) != std::string::npos );
    }

    SECTION( "a narrower bound admits a wider value" )
    {
        // The same literal against a different set: `Floating` admits only f32 and f64, and both
        // hold 128, so what `Integral` refuses this accepts.
        const Typed p( body( "Floating", "a + 128" ) );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the diagnostic names the type doing the rejecting, not the parameter" )
    {
        // `300 does not fit in T` would be unactionable - `T` has no range. Naming the admissible
        // type that cannot hold it is the whole reason the helper returns a Type_id and not a bool.
        const Typed p( body( "Integral", "a + 300" ) );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "the bounds admit `i8`, which cannot hold it" ) != std::string::npos );
    }

    SECTION( "a fractional literal out of `f32`'s range is refused under `Floating`" )
    {
        // f64 always fits - the lexer rejected anything strtod could not hold - so f32 is the only
        // admissible type that can reject, and it has to be consulted for the check to mean
        // anything.
        const Typed p( body( "Floating", "a + 1.0e40" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`f32`" ) != std::string::npos );
    }

    SECTION( "`abs` compiles, which is the point of all of it" )
    {
        const Typed p( "T abs<T>( T a ) where T : Copyable & Numeric { if( a < 0 ) { return 0 - a; } return a; }\n"
                       "i32 main() { return abs<i32>( -7 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

} // namespace keel
#endif
