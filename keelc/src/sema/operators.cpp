#include "sema/operators.h"

#include <fmt/format.h>

#include <optional>

// The operator rules: what each one accepts and what it answers. Every member here takes types and
// spans, never a node, so the expression walk stays above this file.

namespace keel
{
namespace
{

// What an operator accepts, and where its result type comes from. A table rather than nested
// switches, for the same reason the parser keeps binding_power() as one: these are rules, and
// checking them against PLAN §6.4 and D16 should not mean tracing control flow.
enum class Operands : u8
{
    Numeric,    // integers and floats
    Integer,    // integers only
    Bool,       // bools only
    Comparable, // numeric, or two bools
};

// Remove when the KIR can emit the check. `cast` is defined to test the value at run time and
// nothing can do that yet, so a narrowing `cast` would silently truncate - which is `wrap`'s
// behaviour wearing `cast`'s name. Deleting this and the one branch that reads it is the whole
// change; no program that compiles today changes meaning when it goes.
constexpr bool k_narrowing_cast_needs_a_run_time_check = true;

enum class Result : u8
{
    Common, // the §6.4 result of the two operands
    Bool,   // a comparison
    Left,   // the left operand's type - C++ takes no common type for a shift
};

struct Binary_rule
{
    Token_kind kind;
    Operands   operands;
    Result     result;
};

constexpr Binary_rule binary_rules[] = {
    { Token_kind::Plus, Operands::Numeric, Result::Common },
    { Token_kind::Minus, Operands::Numeric, Result::Common },
    { Token_kind::Star, Operands::Numeric, Result::Common },
    { Token_kind::Slash, Operands::Numeric, Result::Common },
    { Token_kind::Percent, Operands::Integer, Result::Common },
    { Token_kind::Amp, Operands::Integer, Result::Common },
    { Token_kind::Pipe, Operands::Integer, Result::Common },
    { Token_kind::Caret, Operands::Integer, Result::Common },
    { Token_kind::Less_less, Operands::Integer, Result::Left },
    { Token_kind::Greater_greater, Operands::Integer, Result::Left },
    { Token_kind::Less, Operands::Numeric, Result::Bool },
    { Token_kind::Less_equal, Operands::Numeric, Result::Bool },
    { Token_kind::Greater, Operands::Numeric, Result::Bool },
    { Token_kind::Greater_equal, Operands::Numeric, Result::Bool },
    { Token_kind::Equal_equal, Operands::Comparable, Result::Bool },
    { Token_kind::Bang_equal, Operands::Comparable, Result::Bool },
    { Token_kind::Amp_amp, Operands::Bool, Result::Bool },
    { Token_kind::Pipe_pipe, Operands::Bool, Result::Bool },
};

struct Unary_rule
{
    Token_kind kind;
    Operands   operands;
    bool       signed_only; // negating an unsigned type has no representable answer
};

constexpr Unary_rule unary_rules[] = {
    { Token_kind::Minus, Operands::Numeric, true },
    { Token_kind::Plus, Operands::Numeric, false },
    { Token_kind::Bang, Operands::Bool, false },
    { Token_kind::Tilde, Operands::Integer, false },
};

const Binary_rule* binary_rule_for( Token_kind kind )
{
    for( const Binary_rule& rule : binary_rules )
    {
        if( rule.kind == kind )
        {
            return &rule;
        }
    }

    return nullptr;
}

const Unary_rule* unary_rule_for( Token_kind kind )
{
    for( const Unary_rule& rule : unary_rules )
    {
        if( rule.kind == kind )
        {
            return &rule;
        }
    }

    return nullptr;
}

// The help line for an operand kind. Empty where the message above it already says enough.
std::string_view operand_requirement( Operands operands )
{
    switch( operands )
    {
    case Operands::Integer:
        return "both operands must be integers";
    case Operands::Bool:
        return "both operands must be `bool`";
    case Operands::Numeric:
    case Operands::Comparable:
        return {};
    }

    return {};
}

// A free function rather than a member: it reads the type table and nothing else.
bool accepts( const Type_table& table, Operands operands, Type_id type )
{
    switch( operands )
    {
    case Operands::Integer:
        return table.is_integer( type );
    case Operands::Numeric:
        return table.is_integer( type ) || table.is_float( type );
    case Operands::Bool:
        return type == table.builtin( Type_kind::Bool );
    case Operands::Comparable:
        return accepts( table, Operands::Numeric, type ) || accepts( table, Operands::Bool, type );
    }

    return false;
}

} // namespace

namespace sema
{

Result_source Operators::result_source( Token_kind op ) const
{
    const Binary_rule* rule = binary_rule_for( op );

    if( rule == nullptr )
    {
        return Result_source::None;
    }

    switch( rule->result )
    {
    case Result::Common:
        return Result_source::Operands;
    case Result::Left:
        return Result_source::Left_operand;
    case Result::Bool:
        break;
    }

    return Result_source::None;
}

Binary_result
Operators::result_of_binary( Token_kind op, Type_id lhs_type, Type_id rhs_type, Node_id current_function, Span at )
{
    const Type_id bool_type = table_.builtin( Type_kind::Bool );

    const auto reject = [&]( std::string_view help ) -> Binary_result
    {
        reporter_.error_at(
            at,
            fmt::format(
                "no operator `{}` for `{}` and `{}`",
                token_kind_spelling( op ),
                table_.name( lhs_type ),
                table_.name( rhs_type )
            ),
            std::string( help )
        );

        return {};
    };

    // D11 checks the body once, before any instance exists, so an operation on a `T` is answered
    // from what the `where` clause promised rather than from any concrete type. Above
    // compares_by_identity because a parameter is neither a pointer nor an enum, so without this it
    // would fall through to the rule table and be rejected by operand classes that have nothing to
    // say about it.
    if( const std::optional<Bound> bound = operator_to_bound( op );
        bound && ( table_.is_parameter( lhs_type ) || table_.is_parameter( rhs_type ) ) )
    {
        const Type_id parameter = table_.is_parameter( lhs_type ) ? lhs_type : rhs_type;
        const Type_id other     = parameter == lhs_type ? rhs_type : lhs_type;

        // D41 generalised (§12): a comparison needs no common type, so a `T` promised to be a
        // number compares against a concrete one exactly as two concrete numbers do. No walk over
        // the types the bound admits, unlike a conversion: every one of them compares against
        // every number, so the answer is `bool` without asking which `T` turns out to be.
        const bool against_a_number =
            is_comparison( op ) && !table_.is_parameter( other ) && accepts( table_, Operands::Numeric, other );

        // Inside a generic body there is no conversion between an unknown type and anything else,
        // so the two sides otherwise have to already agree. Returning rather than reporting and
        // carrying on: one mistake is one diagnostic, and a type recorded past this point would be
        // a claim about an expression that has just been refused.
        if( lhs_type != rhs_type && !against_a_number )
        {
            return reject( fmt::format( "`{}` needs both operands to be the same type", token_kind_spelling( op ) ) );
        }

        // `Numeric` and not the operator's own bound, for the mixed case only. What this needs is
        // not that `T` is orderable but that it is a *number*, which is the one thing `Numeric`
        // says - and the two are not the same promise even where they admit the same types today.
        // `Comparable` and `Equatable` are both satisfied by exactly the numbers only because D33's
        // operators are not shipped; when they are, a type that orders itself satisfies
        // `Comparable` and still does not compare against an `i32`. Gating on `Numeric` is what
        // makes this rule survive that, and it is also what keeps the help honest: an author who
        // wrote `Equatable` wanted equality, and telling them to promise ordering instead would be
        // asking for the wrong thing twice over.
        const Bound required = against_a_number ? Bound::Numeric : *bound;

        if( !bounds_.has_bound( parameter, required ) )
        {
            return reject(
                against_a_number ? fmt::format(
                                       "`{}` must be a number to compare against `{}`; write `where {} : Numeric` on `{}`",
                                       table_.name( parameter ),
                                       table_.name( other ),
                                       table_.name( parameter ),
                                       interner_.text( Symbol_id { ast_.aux( current_function ) } )
                                   )
                                 : fmt::format(
                                       "`{}` needs `{}`; write `where {} : {}` on `{}`",
                                       token_kind_spelling( op ),
                                       name_of_bound( required ),
                                       table_.name( parameter ),
                                       name_of_bound( required ),
                                       interner_.text( Symbol_id { ast_.aux( current_function ) } )
                                   )
            );
        }

        // A comparison answers `bool` whatever `T` turns out to be; arithmetic answers `T`.
        return { against_a_number || *bound == Bound::Equatable || *bound == Bound::Comparable ? bool_type : lhs_type };
    }

    // Pointer and enum equality, which the rule table cannot express - its operand classes are all
    // numeric or bool. For a pointer this is how a null check is written; for an enum it is the
    // only operation there is, since D30 leaves a variant with no conversion to reach arithmetic
    // through. Ordering is deliberately absent from both: comparing pointers into different
    // allocations is meaningless, and an enum's variants are names rather than magnitudes - the
    // declaration order they happen to have is not an ordering anyone wrote down.
    const bool compares_by_identity = table_.is_pointer( lhs_type ) || table_.is_pointer( rhs_type ) ||
                                      table_.is_enum( lhs_type ) || table_.is_enum( rhs_type );

    if( compares_by_identity )
    {
        if( op != Token_kind::Equal_equal && op != Token_kind::Bang_equal )
        {
            return reject( {} );
        }

        if( lhs_type != rhs_type )
        {
            return reject(
                table_.is_enum( lhs_type ) || table_.is_enum( rhs_type ) ? "only values of the same `enum` can be compared"
                                                                         : "only pointers of the same type can be compared"
            );
        }

        return { bool_type };
    }

    const Binary_rule* rule = binary_rule_for( op );

    if( rule == nullptr )
    {
        reporter_.error_at( at, fmt::format( "operator `{}` is not supported yet", token_kind_spelling( op ) ) );
        return {};
    }

    if( !accepts( table_, rule->operands, lhs_type ) || !accepts( table_, rule->operands, rhs_type ) )
    {
        return reject( operand_requirement( rule->operands ) );
    }

    // Two bools compare and combine directly: §6.4 has no row for bool at all.
    if( lhs_type == bool_type && rhs_type == bool_type )
    {
        return { bool_type };
    }

    // D41: a comparison needs no common type. Its answer lands in `bool`, which holds every
    // answer, so the question is about the two values and not about how either is stored -
    // where no one type holds both, lowering compares by cases instead. Arithmetic keeps D5's
    // rule below, because a sum has to land somewhere and `u64 + i64` has nowhere.
    //
    // Both operands numeric, and not just what the rule accepts: `Comparable` also admits bool,
    // and `flag == 1` has no answer of this kind.
    if( rule->result == Result::Bool && accepts( table_, Operands::Numeric, lhs_type ) &&
        accepts( table_, Operands::Numeric, rhs_type ) )
    {
        return { bool_type, true };
    }

    if( rule->result == Result::Left )
    {
        return { lhs_type };
    }

    const Type_id common = table_.arithmetic_result( lhs_type, rhs_type );

    if( !common.is_valid() )
    {
        return reject( {} );
    }

    return { common };
}

Type_id Operators::result_of_unary( Token_kind op, Type_id operand_type, Span at )
{
    const Unary_rule* rule = unary_rule_for( op );

    if( rule == nullptr )
    {
        reporter_.error_at( at, fmt::format( "unary `{}` is not supported yet", token_kind_spelling( op ) ) );
        return {};
    }

    const auto reject = [&]( std::string_view help ) -> Type_id
    {
        reporter_.error_at(
            at,
            fmt::format( "no operator `{}` for `{}`", token_kind_spelling( op ), table_.name( operand_type ) ),
            std::string( help )
        );

        return {};
    };

    if( !accepts( table_, rule->operands, operand_type ) )
    {
        return reject( operand_requirement( rule->operands ) );
    }

    // C++ answers 4294967295 for `-u32(1)`: a silent wrong answer of exactly the kind D5 removes.
    if( rule->signed_only && table_.is_integer( operand_type ) && !table_.get( operand_type ).is_signed )
    {
        return reject( "negation needs a signed type" );
    }

    return operand_type;
}

Type_id Operators::result_of_conditional( Type_id then_type, Type_id else_type, Span at )
{
    if( then_type != else_type )
    {
        reporter_.error_at(
            at,
            fmt::format(
                "ternary branches have different types: `{}` and `{}`", table_.name( then_type ), table_.name( else_type )
            )
        );

        return {};
    }

    return then_type;
}

Conversion_result Operators::convert( bool is_cast, Type_id value, Type_id target, Span at )
{
    const std::string_view name = is_cast ? "cast" : "wrap";
    Type_id                witness;

    // Which side the author wrote a parameter on, if either. `value` wins when both are, matching
    // which side conversion_between draws its witness from.
    const Type_id generic = table_.is_parameter( value ) ? value : table_.is_parameter( target ) ? target : Type_id {};

    const auto reject = [&]( std::string message, std::string help = {} ) -> Conversion_result
    {
        reporter_.error_at( at, std::move( message ), std::move( help ) );

        return {};
    };

    switch( bounds_.conversion_between( value, target, witness ) )
    {
    case Conversion::None:
    {
        // The concrete pair that actually failed. With nothing generic these are the written
        // types, which is why the two tests below did not have to change.
        const Type_id failed_from = table_.is_parameter( value ) ? witness : value;
        const Type_id failed_to   = table_.is_parameter( value ) ? target : witness.is_valid() ? witness : target;

        std::string help;

        // No witness with a parameter in play means the set was not enumerable at all, which is a
        // missing bound rather than a refused pair.
        if( generic.is_valid() && !witness.is_valid() )
        {
            help = fmt::format( "write `where {} : Numeric` to promise it is a number", table_.name( generic ) );
        }
        else if( table_.is_float( failed_from ) && table_.is_integer( failed_to ) )
        {
            help = "rounding is not implied; this needs an explicit rounding function";
        }
        else if( table_.is_integer( failed_from ) && failed_to == table_.builtin( Type_kind::Bool ) )
        {
            help = "compare it instead, as in `x != 0`";
        }

        // `T` is what the author wrote and the witness is what refused it. Only one of the two is
        // something they can act on, so the help carries both.
        if( witness.is_valid() )
        {
            const std::string may = fmt::format( "`{}` may be `{}`", table_.name( generic ), table_.name( witness ) );

            help = help.empty() ? may : fmt::format( "{}, and {}", may, help );
        }

        return reject(
            fmt::format( "`{}` cannot convert `{}` to `{}`", name, table_.name( value ), table_.name( target ) ),
            std::move( help )
        );
    }

    case Conversion::Unsafe:
        // The gate itself is the caller's: the depth it counts belongs to the expression walk.
        return { target, true };

    case Conversion::Cast_only:
        if( !is_cast )
        {
            return reject(
                fmt::format( "`wrap` cannot convert `{}` to `{}`", table_.name( value ), table_.name( target ) ),
                "`wrap` keeps the low bits of an integer; use `cast` here"
            );
        }

        break;

    case Conversion::Both:
        // Narrowing is the one place the two operators disagree, and the check that makes `cast`
        // safe there does not exist yet.
        if( k_narrowing_cast_needs_a_run_time_check && is_cast )
        {
            const Type_id narrows = bounds_.narrowing_witness( value, target );

            if( narrows.is_valid() )
            {
                std::string help =
                    fmt::format( "the run-time check is unimplemented; `wrap<{}>` truncates instead", table_.name( target ) );

                if( generic.is_valid() )
                {
                    help = fmt::format(
                        "`{}` may be `{}`, and {}", table_.name( generic ), table_.name( narrows ), std::move( help )
                    );
                }

                return reject(
                    fmt::format( "`cast` cannot narrow `{}` to `{}` yet", table_.name( value ), table_.name( target ) ),
                    std::move( help )
                );
            }
        }

        break;
    }

    return { target };
}

} // namespace sema
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

// An operator rule asked of two Type_ids, with no source string anywhere - which is what taking
// types rather than nodes buys, and what the rest of sema/ still cannot do. Five collaborators is
// what constructing the question costs.
TEST_CASE( "operators_answer_the_ternary_from_two_types_alone", "[sema][operator]" )
{
    Ast            ast;
    Interner       interner;
    Source_manager sm;
    Diagnostics    diags;
    Literal_pool   pool;

    sema::Reporter      reporter( sm, diags );
    sema::Types_builder types;
    sema::Aggregates    aggregates( ast, interner, types, reporter );
    sema::Bounds        bounds( ast, interner, pool, types, aggregates, reporter );
    sema::Operators     operators( ast, interner, types.table(), bounds, reporter );

    const Type_id i32 = types.table().integer( 32, true );
    const Type_id i64 = types.table().integer( 64, true );

    SECTION( "two arms of one type answer that type" )
    {
        REQUIRE( operators.result_of_conditional( i32, i32, Span {} ) == i32 );
        REQUIRE( diags.error_count() == 0 );
    }

    SECTION( "two arms of different types are refused, and nothing is widened to reconcile them" )
    {
        REQUIRE( !operators.result_of_conditional( i32, i64, Span {} ).is_valid() );
        REQUIRE( diags.error_count() == 1 );
    }
}

// Three rules about types alone, asked of the types directly: nothing pinned them while the only
// way to ask was a source string.
TEST_CASE( "operators_refuse_to_negate_an_unsigned_type", "[sema][operator]" )
{
    Ast            ast;
    Interner       interner;
    Source_manager sm;
    Diagnostics    diags;
    Literal_pool   pool;

    sema::Reporter      reporter( sm, diags );
    sema::Types_builder types;
    sema::Aggregates    aggregates( ast, interner, types, reporter );
    sema::Bounds        bounds( ast, interner, pool, types, aggregates, reporter );
    sema::Operators     operators( ast, interner, types.table(), bounds, reporter );

    // D5: C++ answers 4294967295 for `-u32( 1 )`, which is the silent wrong answer Keel refuses.
    SECTION( "an unsigned operand has no representable negation" )
    {
        REQUIRE( !operators.result_of_unary( Token_kind::Minus, types.table().integer( 32, false ), Span {} ).is_valid() );
        REQUIRE( diags.error_count() == 1 );
    }

    SECTION( "a signed one keeps its own type" )
    {
        const Type_id i32 = types.table().integer( 32, true );

        REQUIRE( operators.result_of_unary( Token_kind::Minus, i32, Span {} ) == i32 );
        REQUIRE( diags.error_count() == 0 );
    }

    // The other three the table holds, so that the rule above is known to be Minus' alone.
    SECTION( "the other unary operators take their operand's type" )
    {
        const Type_id u32       = types.table().integer( 32, false );
        const Type_id bool_type = types.table().builtin( Type_kind::Bool );

        REQUIRE( operators.result_of_unary( Token_kind::Plus, u32, Span {} ) == u32 );
        REQUIRE( operators.result_of_unary( Token_kind::Tilde, u32, Span {} ) == u32 );
        REQUIRE( operators.result_of_unary( Token_kind::Bang, bool_type, Span {} ) == bool_type );
        REQUIRE( diags.error_count() == 0 );
    }
}

// §6.4: a shift is the one operator that takes no common type. Its count is a width rather than a
// value in the same type, so the left operand decides alone - which is both what the result is and
// which operand an expectation may reach.
TEST_CASE( "operators_take_a_shift_from_its_left_operand_only", "[sema][operator]" )
{
    Ast            ast;
    Interner       interner;
    Source_manager sm;
    Diagnostics    diags;
    Literal_pool   pool;

    sema::Reporter      reporter( sm, diags );
    sema::Types_builder types;
    sema::Aggregates    aggregates( ast, interner, types, reporter );
    sema::Bounds        bounds( ast, interner, pool, types, aggregates, reporter );
    sema::Operators     operators( ast, interner, types.table(), bounds, reporter );

    const Type_id u8  = types.table().integer( 8, false );
    const Type_id i32 = types.table().integer( 32, true );

    SECTION( "the result is the left operand's, not the common type" )
    {
        REQUIRE( operators.result_of_binary( Token_kind::Less_less, u8, i32, Node_id {}, Span {} ).type == u8 );
        REQUIRE( operators.result_of_binary( Token_kind::Greater_greater, u8, i32, Node_id {}, Span {} ).type == u8 );
        REQUIRE( diags.error_count() == 0 );
    }

    SECTION( "and an expectation reaches that operand alone" )
    {
        REQUIRE( operators.result_source( Token_kind::Less_less ) == sema::Result_source::Left_operand );
        REQUIRE( operators.result_source( Token_kind::Plus ) == sema::Result_source::Operands );
        REQUIRE( operators.result_source( Token_kind::Less ) == sema::Result_source::None );
        REQUIRE( operators.result_source( Token_kind::Question ) == sema::Result_source::None );
    }
}

// The help for a refused generic conversion carries two types: the `T` the author wrote, which they
// cannot act on, and the admissible member that refused it, which they can. The goldens cover every
// shape of this; one case here keeps the unit suite honest about the witness appearing at all.
TEST_CASE( "operators_name_the_admissible_type_that_refused_a_conversion", "[sema][operator][cast]" )
{
    const Typed p( "i32 f<T>( T v ) where T : Copyable & Numeric { return cast<i32>( v ); }\ni32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "`T` may be `f32`" ) != std::string::npos );
    REQUIRE( p.rendered().find( "rounding is not implied" ) != std::string::npos );
}

// D5 / §6.4 reaching the surface. The table itself is tested in type.cpp; these check that the
// checker consults it, and on the right pair of types.
TEST_CASE( "type_checker_applies_the_conversion_table", "[sema][types]" )
{
    SECTION( "lossless widening needs no cast" )
    {
        const Typed p( "i64 f( i32 a, i64 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "i64" );
    }

    SECTION( "a strictly wider signed type absorbs an unsigned one" )
    {
        const Typed p( "i32 f( i32 a, u8 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "i32" );
    }

    SECTION( "equal-rank mixed signedness is rejected - the case C++ gets wrong" )
    {
        const Typed p( "i64 f( i32 a, u32 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "so is a float that cannot hold the integer exactly" )
    {
        const Typed p( "f64 f( i64 a, f64 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
    }

    SECTION( "same-type arithmetic stays in the type" )
    {
        const Typed p( "u8 f( u8 a, u8 b ) { return a + b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "u8" );
    }
}

// A comparison yields bool, and D41 is the rule that its operands need not meet: the answer lands
// in a type that holds every answer, so no common type is needed to produce one.
TEST_CASE( "type_checker_types_comparisons_as_bool", "[sema][types]" )
{
    SECTION( "the result is bool, not the operand type" )
    {
        const Typed p( "bool f( i32 a, i32 b ) { return a < b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "bool" );
    }

    // The cell §6.4 refuses for arithmetic. C++ compiles it and answers wrongly, which is the
    // reason D41 is a divergence worth taking rather than a relaxation.
    SECTION( "D41: mixed signedness compares where it cannot add" )
    {
        const Typed p( "bool f( i32 a, u32 b ) { return a < b; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "bool" );
    }

    // No type holds both, and it makes no difference: the question still has an answer.
    SECTION( "D41: and where no type holds both operands" )
    {
        static const char* accepted[] = {
            "bool f( i64 a, u64 b ) { return a < b; }",
            "bool f( i64 a, f64 b ) { return a >= b; }",
            "bool f( u64 a, f32 b ) { return a == b; }",
            "bool f( f32 a, i64 b ) { return a != b; }",
        };

        for( const char* source : accepted )
        {
            const Typed p( source );

            INFO( "source: " << source << "\n" << p.rendered() );
            REQUIRE( p.clean() );
            REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "bool" );
        }
    }

    // D41 moves the line for comparison alone. Arithmetic on the same pair still has nowhere to
    // put its result, which is the whole of why the two rules differ.
    SECTION( "D41 does not reach arithmetic" )
    {
        static const char* rejected[] = {
            "u32 f( i32 a, u32 b ) { return a + b; }",
            "i64 f( i64 a, u64 b ) { return a * b; }",
            "f64 f( i64 a, f64 b ) { return a - b; }",
        };

        for( const char* source : rejected )
        {
            const Typed p( source );

            INFO( "source: " << source << "\n" << p.rendered() );
            REQUIRE( p.errors() >= 1 );
        }
    }

    // `Comparable` admits bool, so the operand check alone would let `flag == 1` through to a
    // rule that has no answer for it. Numeric on both sides is what the new branch tests.
    SECTION( "a bool still compares only against a bool" )
    {
        static const char* rejected[] = {
            "bool f( bool a, i32 b ) { return a == b; }",
            "bool f( i32 a, bool b ) { return a != b; }",
        };

        for( const char* source : rejected )
        {
            const Typed p( source );

            INFO( "source: " << source << "\n" << p.rendered() );
            REQUIRE( p.errors() >= 1 );
        }
    }
}

// The conversion table. `cast` preserves the value and `wrap` keeps the low bits, so the two
// accept genuinely different sets - a cell that both allow, or that neither does, is the
// interesting part rather than an accident.
TEST_CASE( "type_checker_allows_the_conversions_in_the_table", "[sema][types][cast]" )
{
    SECTION( "cast widens an integer" )
    {
        const Typed p( "i32 main() { i32 x = 1; i64 y = cast<i64>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Cast_expr, 0 ) ) == "i64" );
    }

    SECTION( "cast converts an integer to a float" )
    {
        const Typed p( "i32 main() { i32 x = 1; f32 y = cast<f32>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Precision loss, but a float that cannot hold the value gives an infinity rather than a
    // plausible wrong number, so this is not the case narrowing is held back for.
    SECTION( "cast narrows a float" )
    {
        const Typed p( "i32 main() { f64 d = 1.5; f32 y = cast<f32>( d ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "cast converts a bool to an integer" )
    {
        const Typed p( "i32 main() { bool b = true; i32 y = cast<i32>( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "wrap narrows an integer" )
    {
        const Typed p( "i32 main() { i32 x = 300; u8 y = wrap<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.type_name( p.nth( Node_kind::Cast_expr, 0 ) ) == "u8" );
    }

    // Losing the sign is exactly what wrap is for, and exactly what cast must refuse.
    SECTION( "wrap reinterprets the sign" )
    {
        const Typed p( "i32 main() { i32 x = 0 - 1; u32 y = wrap<u32>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "wrap widens too, and simply never wraps" )
    {
        const Typed p( "i32 main() { i32 x = 1; i64 y = wrap<i64>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_rejects_the_conversions_outside_the_table", "[sema][types][cast]" )
{
    // No one obvious rounding, so neither operator picks one.
    SECTION( "neither converts a float to an integer" )
    {
        for( const char* tail : { "i32 y = cast<i32>( d );", "i32 y = wrap<i32>( d );" } )
        {
            const Typed p( std::string( "i32 main() { f64 d = 1.5; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( "rounding" ) != std::string::npos );
        }
    }

    // `x != 0` says it better, and modular arithmetic down to one bit says something else again.
    SECTION( "neither converts an integer to a bool" )
    {
        for( const char* tail : { "bool y = cast<bool>( x );", "bool y = wrap<bool>( x );" } )
        {
            const Typed p( std::string( "i32 main() { i32 x = 1; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    SECTION( "wrap refuses anything that is not integer to integer" )
    {
        for( const char* source :
             { "i32 main() { i32 x = 1; f32 y = wrap<f32>( x ); return 0; }",
               "i32 main() { bool b = true; i32 y = wrap<i32>( b ); return 0; }",
               "i32 main() { f64 d = 1.5; f32 y = wrap<f32>( d ); return 0; }" } )
        {
            const Typed p( source );

            INFO( source << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( "`wrap` cannot convert" ) != std::string::npos );
        }
    }

    SECTION( "neither touches a struct" )
    {
        for( const char* tail : { "i32 y = cast<i32>( p );", "i32 y = wrap<i32>( p );" } )
        {
            const Typed p( std::string( "struct P { i32 x; };\ni32 main() { P p = P { 1 }; " ) + tail + " return 0; }" );

            INFO( tail << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
        }
    }

    SECTION( "converting to a struct is refused too" )
    {
        const Typed p( "struct P { i32 x; };\ni32 main() { i32 x = 1; P y = cast<P>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // A real conversion rather than nonsense, so it is gated rather than refused - the one cell of
    // the table `unsafe` opens. See the [unsafe] cases for the gate itself.
    SECTION( "a pointer conversion needs an unsafe block" )
    {
        const Typed p( "i32 main() { i32 x = 1; i32* q = &x; u8* r = cast<u8*>( q ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`unsafe` block" ) != std::string::npos );
    }

    SECTION( "an unknown target type is reported once" )
    {
        const Typed p( "i32 main() { i32 x = 1; return cast<Nope>( x ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// Narrowing is the one cell where the two operators disagree, and `cast` is defined to check the
// value at run time. Nothing can do that yet, so it is refused rather than silently truncating -
// which would be `wrap`'s behaviour under `cast`'s name. Delete these when the check exists.
TEST_CASE( "type_checker_holds_back_a_narrowing_cast", "[sema][types][cast]" )
{
    SECTION( "narrowing the width" )
    {
        const Typed p( "i32 main() { i32 x = 300; u8 y = cast<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot narrow" ) != std::string::npos );
        REQUIRE( p.rendered().find( "wrap<u8>" ) != std::string::npos );
    }

    // u32 cannot hold a negative i32, so this narrows even though the widths match.
    SECTION( "changing the signedness" )
    {
        const Typed p( "i32 main() { i32 x = 1; u32 y = cast<u32>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot narrow" ) != std::string::npos );
    }

    SECTION( "but wrap says the same thing and is allowed" )
    {
        const Typed p( "i32 main() { i32 x = 300; u8 y = wrap<u8>( x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Equality only, and only within one enum. Ordering is absent deliberately: variants are names
// rather than magnitudes, so the order they were declared in is not an order anyone wrote down.
TEST_CASE( "type_checker_compares_enums_by_identity", "[sema][enum]" )
{
    SECTION( "two values of one enum compare" )
    {
        const Typed p( "enum Colour { Red, Green };\n"
                       "i32 main() { Colour c = Colour::Red; bool b = c == Colour::Green; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "two different enums do not" )
    {
        const Typed p( "enum A { X };\nenum B { Y };\ni32 main() { bool b = A::X == B::Y; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "only values of the same `enum` can be compared" ) != std::string::npos );
    }

    SECTION( "and ordering is not defined" )
    {
        const Typed p( "enum Colour { Red, Green };\ni32 main() { bool b = Colour::Red < Colour::Green; return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// D40. A bound is a promise about a parameter, and a type argument either keeps it or does not.
// The two halves are has_bound (what `T` promises) and satisfies (what a concrete type delivers),
// and every case below is one or the other answering at a call site.
//
// `const ref T` throughout rather than `T`: the by-value binding mode of an owning parameter is
// D31's question, not this one, and mixing them would make a failure here ambiguous.
// D11 again, from the operator side: an operation on a `T` is answered from what the `where` clause
// promised, before any instance exists. The pre-check sits above the pointer-and-enum rule because a
// parameter is neither, and would otherwise be judged by operand classes that say nothing about it.
TEST_CASE( "type_checker_checks_operators_on_a_type_parameter", "[sema][generic][bound]" )
{
    SECTION( "an operator the clause promised is accepted" )
    {
        const Typed comparison( "bool f<T>( T a ) where T : Comparable { return a < a; }\ni32 main() { return 0; }" );
        const Typed equality( "bool f<T>( T a ) where T : Equatable { return a == a; }\ni32 main() { return 0; }" );
        const Typed arithmetic( "T f<T>( T a ) where T : Numeric { return a + a; }\ni32 main() { return 0; }" );
        const Typed bitwise( "T f<T>( T a ) where T : Integral { return a & a; }\ni32 main() { return 0; }" );

        INFO( comparison.rendered() << equality.rendered() << arithmetic.rendered() << bitwise.rendered() );
        REQUIRE( comparison.clean() );
        REQUIRE( equality.clean() );
        REQUIRE( arithmetic.clean() );
        REQUIRE( bitwise.clean() );
    }

    SECTION( "a comparison answers `bool`, and arithmetic answers `T`" )
    {
        const Typed p( "T f<T>( T a ) where T : Numeric { bool b = a < a; T c = a + a; return c; }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an operator it did not names the bound and the clause to write" )
    {
        const Typed p( "bool f<T>( T a ) { return a == a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no operator `==` for `T` and `T`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`==` needs `Equatable`; write `where T : Equatable` on `f`" ) != std::string::npos );
    }

    SECTION( "implication reaches the operator table" )
    {
        // `Integral` was stored with `Numeric`, `Comparable` and `Equatable`, so one clause buys all
        // four operators; `Comparable` does not run downhill to arithmetic.
        const Typed widest( "bool f<T>( T a ) where T : Integral { return a < a; }\ni32 main() { return 0; }" );
        const Typed narrow( "T f<T>( T a ) where T : Comparable { return a + a; }\ni32 main() { return 0; }" );

        INFO( widest.rendered() << narrow.rendered() );
        REQUIRE( widest.clean() );
        REQUIRE( narrow.errors() == 1 );
        REQUIRE( narrow.rendered().find( "`+` needs `Numeric`" ) != std::string::npos );
    }

    SECTION( "operands that are not the same type are one diagnostic, not two" )
    {
        // The mismatch returns rather than reporting and carrying on. Without that, a `T` that does
        // hold the bound goes on to record a type on an expression just refused - and one that does
        // not reports the same mistake twice.
        //
        // Against a bool, because D41 lets a numeric `T` meet a concrete *number* - this is the
        // mismatch that survives it.
        const Typed unbounded( "bool f<T>( T a ) { bool x = true; return a == x; }\ni32 main() { return 0; }" );
        const Typed bounded( "i32 f<T>( T a ) where T : Equatable { bool x = true; i32 y = a == x; return y; }\n"
                             "i32 main() { return 0; }" );

        INFO( unbounded.rendered() << bounded.rendered() );
        REQUIRE( unbounded.errors() == 1 );
        REQUIRE( unbounded.rendered().find( "needs both operands to be the same type" ) != std::string::npos );
        REQUIRE( bounded.errors() == 1 );
        REQUIRE( bounded.rendered().find( "expected `i32`, but got `bool`" ) == std::string::npos );
    }

    SECTION( "the help names the parameter whichever side it was on" )
    {
        // `where i32 : Comparable` is not a thing anyone can write, and the number is on the left
        // here - so the help has to read the parameter off the other operand.
        const Typed p( "bool f<T>( T a ) { i32 x = 1; return x < a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE(
            p.rendered().find( "`T` must be a number to compare against `i32`; write `where T : Numeric` on `f`" ) !=
            std::string::npos
        );
        REQUIRE( p.rendered().find( "where i32 :" ) == std::string::npos );
    }

    // D41 generalised (§12). The bound is what makes it legal, not the operator - an `Equatable`
    // `T` may be an enum or a pointer, and neither of those compares against a number.
    SECTION( "a numeric `T` compares against a concrete number" )
    {
        static const char* accepted[] = {
            "bool f<T>( T a ) where T : Numeric { i32 x = 1; return a < x; }",
            "bool f<T>( T a ) where T : Floating { f64 x = 1.5; return x >= a; }",
            "bool f<T>( T a ) where T : Integral { u64 x = 1; return a != x; }",
            "bool f<T>( T a ) where T : Floating { i64 x = 1; return a == x; }",
        };

        for( const char* source : accepted )
        {
            const std::string source_with_main = std::string( source ) + "\ni32 main() { return 0; }";
            const Typed       p( source_with_main.c_str() );

            INFO( "source: " << source << "\n" << p.rendered() );
            REQUIRE( p.clean() );
            REQUIRE( p.type_name( p.nth( Node_kind::Binary_expr, 0 ) ) == "bool" );
        }
    }

    SECTION( "but arithmetic against one is still refused" )
    {
        const Typed p( "T f<T>( T a ) where T : Numeric { i32 x = 1; return a + x; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() >= 1 );
        REQUIRE( p.rendered().find( "needs both operands to be the same type" ) != std::string::npos );
    }

    // Two parameters is a question §12 did not answer - it decided a `T` against a *concrete*
    // number. Every admissible pair would compare, so this is scope rather than soundness, and it
    // stays refused until something asks for it.
    SECTION( "but not one `T` against another" )
    {
        const Typed p( "bool f<T, U>( T a, U b ) where T : Numeric, where U : Numeric { return a < b; }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "needs both operands to be the same type" ) != std::string::npos );
    }

    // The bound has to say `T` is a *number*, and neither of these does. `Comparable` promises
    // ordering and `Equatable` promises equality; both are satisfied by exactly the numbers only
    // because D33's operators are not shipped, and neither is the promise this rule needs.
    SECTION( "and a bound that is not `Numeric` does not buy one" )
    {
        static const char* rejected[] = {
            "bool f<T>( T a ) where T : Equatable { i32 x = 1; return a == x; }",
            "bool f<T>( T a ) where T : Comparable { i32 x = 1; return a < x; }",
        };

        for( const char* source : rejected )
        {
            const std::string source_with_main = std::string( source ) + "\ni32 main() { return 0; }";
            const Typed       p( source_with_main.c_str() );

            INFO( "source: " << source << "\n" << p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE(
                p.rendered().find( "`T` must be a number to compare against `i32`; write `where T : Numeric` on `f`" ) !=
                std::string::npos
            );
        }
    }

    SECTION( "an operator no bound grants is left to the rule table" )
    {
        // `&&` and `||` want bools and nothing in D40's set makes a `T` one, so the pre-check has
        // nothing to add and says so by declining to answer rather than by asserting.
        const Typed p( "bool f<T>( T a ) where T : Comparable { return a && a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no operator `&&` for `T` and `T`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "both operands must be `bool`" ) != std::string::npos );
    }
}

} // namespace keel
#endif
