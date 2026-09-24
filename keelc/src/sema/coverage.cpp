#include "sema/coverage.h"

#include <fmt/format.h>

#include <algorithm>
#include <limits>

// Whether a `switch`'s labels account for its scrutinee, and whether its arms are shaped for a
// language whose `case` does not run on. coverage.h says why the arm loop is the caller's.

namespace keel
{

namespace
{

// "`A`", "`A` and `B`", "`A`, `B` and `C`" - a list a person would read aloud. Worth the twelve
// lines because the message it builds, naming the variants a `switch` has missed, is the one
// exhaustiveness checking exists to produce: "not exhaustive" would leave the author to work out
// which, which is the work the compiler just did.
std::string quoted_list( std::span<const std::string_view> names )
{
    std::string text;

    for( std::size_t i = 0; i < names.size(); ++i )
    {
        if( i != 0 )
        {
            text += i + 1 == names.size() ? " and " : ", ";
        }

        text += fmt::format( "`{}`", names[i] );
    }

    return text;
}

} // namespace

namespace sema
{

Switch_coverage Coverage::begin_switch( Node_id id, Type_id type )
{
    Switch_coverage state;

    state.switch_id = id;
    state.type      = type;
    state.numeric   = table_.is_integer( type ) || table_.is_float( type );
    state.floating  = table_.is_float( type );

    // The label that covered each ordinal, so a duplicate can point at the first one and a gap is
    // nameable: the missing variants are the entries nothing wrote to.
    if( !state.numeric )
    {
        state.variants.assign( ast_.variants( table_.get( type ).declaration ).size(), Node_id {} );
    }

    return state;
}

void Coverage::check_arm_labels( Node_id arm, Switch_coverage& state )
{
    state.has_default = state.has_default || ast_.aux( arm ) == 1;

    if( state.numeric )
    {
        check_numeric_arm_labels( arm, state );
        return;
    }

    check_enum_arm_labels( arm, state );
}

void Coverage::finish_switch( const Switch_coverage& state )
{
    if( state.numeric )
    {
        finish_numeric_switch( state );
        return;
    }

    finish_enum_switch( state );
}

void Coverage::check_enum_arm_labels( Node_id arm, Switch_coverage& state )
{
    std::vector<Node_id>& covered = state.variants;

    const std::span<const Node_id> arm_children = ast_.children( arm );

    // The last child is the body; everything before it is a label.
    for( const Node_id label : arm_children.subspan( 0, arm_children.size() - 1 ) )
    {
        // D7: a pattern names a variant *and* binds its payload. The path inside it is what
        // covers the variant, so coverage is asked of that and the bindings are declared into the
        // arm's scope - which is why the caller visits the body only after this returns.
        if( ast_.kind( label ) == Node_kind::Variant_pattern )
        {
            check_variant_pattern( label, state.type, covered );
            continue;
        }

        // D34's ranges are over numbers. A range of variants would need declaration order to be
        // an *ordering*, which is exactly what enum comparison already refuses to treat it as.
        if( ast_.kind( label ) == Node_kind::Range_expr )
        {
            reporter_.error_at(
                ast_.span( label ),
                "a range cannot name variants",
                "the order variants were declared in is not an ordering; list them instead"
            );

            continue;
        }

        // A bare label names the variant and ignores its payload, which is a complete thing to
        // say here even when the variant carries one - so the same entry point a pattern uses.
        // `Shape s = Shape::Circle;` takes the ordinary path and still reports.
        const Type_id label_type = expressions_.infer_in_pattern( label, state.type );

        if( table_.is_error( label_type ) )
        {
            continue;
        }

        if( label_type != state.type )
        {
            reporter_.error_at(
                ast_.span( label ), fmt::format( "a `case` label must be a variant of `{}`", table_.name( state.type ) )
            );

            continue;
        }

        // infer_path recorded the ordinal, which is what makes coverage a vector index rather
        // than a search.
        const std::optional<Constant_value> value = constant_folder_.value_of( label );

        if( !value || value->magnitude >= covered.size() )
        {
            continue;
        }

        const std::size_t ordinal = static_cast<std::size_t>( value->magnitude );

        if( covered[ordinal].is_valid() )
        {
            reporter_.error_at(
                ast_.span( label ),
                fmt::format( "`{}` is already covered", interner_.text( Symbol_id { ast_.aux( label ) } ) ),
                reporter_.previous_declaration_note( ast_.span( covered[ordinal] ) )
            );

            continue;
        }

        covered[ordinal] = label;
    }
}

void Coverage::finish_enum_switch( const Switch_coverage& state )
{
    const std::span<const Node_id> variants = ast_.variants( table_.get( state.type ).declaration );
    const std::vector<Node_id>&    covered  = state.variants;

    // Named rather than counted: "missing `Green` and `Blue`" is the diagnostic this whole feature
    // exists to produce, and "not exhaustive" would leave the author to work it out.
    std::vector<std::string_view> missing;

    for( std::size_t i = 0; i < variants.size(); ++i )
    {
        if( !covered[i].is_valid() )
        {
            missing.push_back( interner_.text( Symbol_id { ast_.aux( variants[i] ) } ) );
        }
    }

    if( !missing.empty() && !state.has_default )
    {
        reporter_.error_at(
            ast_.span( state.switch_id ),
            fmt::format( "`switch` does not cover every variant of `{}`", table_.name( state.type ) ),
            fmt::format( "missing {}", quoted_list( missing ) )
        );
    }

    // D15's shape: a construct with no effect is an error rather than a warning. A `default` here
    // can never run, and leaving it would mean a variant added later lands in it silently - which
    // is the guarantee the whole construct exists for.
    if( missing.empty() && state.has_default )
    {
        reporter_.error_at(
            ast_.span( state.switch_id ),
            fmt::format( "every variant of `{}` is covered, so `default` can never run", table_.name( state.type ) ),
            "remove it, so that adding a variant later is a compile error here"
        );
    }
}

// Coverage as a set of half-open intervals. Overlap is decidable and reported; completeness is not,
// so `default` is required rather than inferred.
void Coverage::check_numeric_arm_labels( Node_id arm, Switch_coverage& state )
{
    std::vector<Interval>& covered = state.intervals;

    const std::span<const Node_id> arm_children = ast_.children( arm );

    for( const Node_id label : arm_children.subspan( 0, arm_children.size() - 1 ) )
    {
        const bool is_range = ast_.kind( label ) == Node_kind::Range_expr;

        // Exact equality against a float is the mistake every float guide opens with, and a
        // `case` is the one place it would look deliberate. A range says what was meant.
        if( state.floating && !is_range )
        {
            reporter_.error_at(
                ast_.span( label ),
                fmt::format( "a single value cannot be matched against a `{}`", table_.name( state.type ) ),
                "comparing floats for equality is rarely what is meant; write a range, as in `case 1..2:`"
            );

            continue;
        }

        Interval interval { .label = label };

        if( is_range )
        {
            const std::optional<i64> low  = fold_bound( ast_.child( label, 0 ), state.type );
            const std::optional<i64> high = fold_bound( ast_.child( label, 1 ), state.type );

            if( !low.has_value() || !high.has_value() )
            {
                continue;
            }

            // Half-open, so `low == high` is empty and `low > high` is backwards. Both are
            // certainly a mistake, and both are visible without running anything - which is the
            // whole of what D34 asks the checker to catch.
            if( *low >= *high )
            {
                reporter_.error_at(
                    ast_.span( label ),
                    fmt::format( "`{}..{}` matches nothing", *low, *high ),
                    *low == *high ? "a range excludes its upper bound, so this one is empty"
                                  : "a range runs upwards; the lower bound comes first"
                );

                continue;
            }

            interval.low  = *low;
            interval.high = *high;
        }
        else
        {
            const std::optional<i64> value = fold_bound( label, state.type );

            if( !value.has_value() )
            {
                continue;
            }

            // A single value is the interval it covers, so everything below treats the two the
            // same.
            interval.low  = *value;
            interval.high = *value + 1;
        }

        // Pairwise rather than sorted: a `switch` has a handful of arms, and reporting against
        // the label that was written first needs the source order anyway.
        const auto clash = std::find_if(
            covered.begin(),
            covered.end(),
            [&]( const Interval& other ) { return interval.low < other.high && other.low < interval.high; }
        );

        if( clash != covered.end() )
        {
            reporter_.error_at(
                ast_.span( label ),
                "this `case` overlaps an earlier one",
                reporter_.previous_declaration_note( ast_.span( clash->label ) )
            );

            continue;
        }

        covered.push_back( interval );
    }
}

void Coverage::finish_numeric_switch( const Switch_coverage& state )
{
    // No completeness check and no dead-`default` rule: neither is decidable over a number, and a
    // `default` that looks redundant here cannot be proved so.
    if( !state.has_default )
    {
        reporter_.error_at(
            ast_.span( state.switch_id ),
            fmt::format( "a `switch` over `{}` needs a `default`", table_.name( state.type ) ),
            "its values cannot all be listed, so there is no other way to be exhaustive"
        );
    }
}

// Coverage as a vector indexed by ordinal, which is what makes a gap nameable: the missing variants
// are the entries nothing wrote to.
// D7. `case Shape::Circle( r ):` covers `Circle` and binds `r` to its payload. The binding is
// read-only: today the payload is copied, and when owning payloads arrive it becomes a borrow - at
// which point writing through it would be writing into a value the enum still owns.
void Coverage::check_variant_pattern( Node_id pattern, Type_id type, std::vector<Node_id>& covered )
{
    const std::span<const Node_id> parts    = ast_.children( pattern );
    const Node_id                  path     = parts[0];
    const std::span<const Node_id> bindings = parts.subspan( 1 );

    // The pattern accounts for the payload, so the path inside it names a variant rather than
    // standing for a value.
    const Type_id path_type = expressions_.infer_in_pattern( path, type );

    if( table_.is_error( path_type ) )
    {
        return;
    }

    if( path_type != type )
    {
        reporter_.error_at( ast_.span( path ), fmt::format( "a `case` label must be a variant of `{}`", table_.name( type ) ) );
        return;
    }

    const std::optional<Constant_value> ordinal = constant_folder_.value_of( path );

    if( !ordinal || ordinal->magnitude >= covered.size() )
    {
        return;
    }

    const std::size_t index = static_cast<std::size_t>( ordinal->magnitude );

    if( covered[index].is_valid() )
    {
        reporter_.error_at(
            ast_.span( path ),
            fmt::format( "`{}` is already covered", interner_.text( Symbol_id { ast_.aux( path ) } ) ),
            reporter_.previous_declaration_note( ast_.span( covered[index] ) )
        );

        return;
    }

    covered[index] = path;

    const Node_id                  decl    = table_.get( type ).declaration;
    const Node_id                  variant = ast_.variants( decl )[index];
    const std::span<const Node_id> payload = ast_.children( variant );

    if( payload.size() != bindings.size() )
    {
        reporter_.error_at(
            ast_.span( pattern ),
            fmt::format(
                "`{}` carries {} value{}, but {} {} bound",
                interner_.text( Symbol_id { ast_.aux( variant ) } ),
                payload.size(),
                payload.size() == 1 ? "" : "s",
                bindings.size(),
                bindings.size() == 1 ? "was" : "were"
            ),
            payload.empty() ? "write it without a pattern" : "bind a name for each field of the payload"
        );
    }

    // Each binding takes its field's type *through this instance*: what the declaration records for
    // `Opt<T>`'s payload is a `T`. The resolver has already put the names in the arm's scope, so all
    // that is left here is to say what they hold.
    const std::size_t shared = std::min( payload.size(), bindings.size() );

    for( std::size_t i = 0; i < shared; ++i )
    {
        // A pattern's bindings are the one thing this class writes a type for.
        types_.record( bindings[i], aggregates_.field_type( type, payload[i] ) );
    }
}

// A `case` bound is always an integer, whatever the scrutinee is - D34 gives `..` one meaning, and
// the meaning it will need for slicing is an integer one. So `case 1..5:` reads the same over an
// `f64` as over an `i32`, and `1.5..7.3` has no spelling.
std::optional<i64> Coverage::fold_bound( Node_id bound, Type_id scrutinee )
{
    // Types the bound as well as reading it: fold_integer only folds, and lowering needs the node
    // to carry a type. Against the *scrutinee's* type, so `case 1..5:` over an `f64` compares
    // against 1.0 and 5.0 rather than needing a cast nobody wrote.
    expressions_.check( bound, scrutinee );

    const Folded folded = constant_folder_.fold_integer( bound );

    if( !folded.constant )
    {
        reporter_.error_at(
            ast_.span( bound ),
            "a `case` label must be known at compile time",
            "a `switch` decides which arm runs by comparing constants"
        );

        return std::nullopt;
    }

    // The interval arithmetic below is signed, and a label at the very top of `u64` is not worth
    // widening it for - saying so is better than being quietly wrong about the overlap.
    if( folded.overflowed || folded.value.magnitude > static_cast<u64>( std::numeric_limits<i64>::max() ) )
    {
        reporter_.error_at( ast_.span( bound ), "this `case` label is too large to check for overlap" );
        return std::nullopt;
    }

    const i64 magnitude = static_cast<i64>( folded.value.magnitude );

    return folded.value.negative ? -magnitude : magnitude;
}

bool Coverage::arm_binds( Node_id arm ) const
{
    const std::span<const Node_id> children = ast_.children( arm );

    for( const Node_id label : children.subspan( 0, children.size() - 1 ) )
    {
        // A Variant_pattern is the only label form that introduces names, and it carries one
        // Binding_decl per bound field - so an empty one binds nothing despite being a pattern.
        if( ast_.kind( label ) == Node_kind::Variant_pattern && ast_.children( label ).size() > 1 )
        {
            return true;
        }
    }

    return false;
}

// Three rules about the shape of an arm. All of them exist because a `switch` is the one place
// Keel's meaning and C++'s can differ for the same source: an arm that runs on into the next in
// C++ simply ends here, and a label that binds gives names to fields the matched variant may not
// have.
void Coverage::check_arm_structure( Node_id id )
{
    const std::span<const Node_id> arms = ast_.children( id ).subspan( 1 );

    for( std::size_t i = 0; i < arms.size(); ++i )
    {
        const Node_id                  arm      = arms[i];
        const std::span<const Node_id> children = ast_.children( arm );
        const Node_id                  body     = children.back();
        const bool                     last     = i + 1 == arms.size();

        // Stacked labels share one body, so they must agree on what is in scope in it. The only
        // way to agree is to bind nothing: `case Circle( r ): case Rect( w, h ):` would read `r`
        // out of a Rect, which is a field that variant does not have.
        if( children.size() > 2 && arm_binds( arm ) )
        {
            reporter_.error_at(
                ast_.span( arm ),
                "a `case` that destructures cannot share its body with another",
                "give it an arm of its own, or drop the `( ... )` to match the variant without its payload"
            );
        }

        // `fallthrough` is the arm's last act. Anywhere else and what follows is unreachable, and
        // "the next arm" stops being the obvious reading.
        const std::span<const Node_id> statements =
            ast_.kind( body ) == Node_kind::Block ? ast_.children( body ) : std::span<const Node_id> {};

        for( std::size_t s = 0; s < statements.size(); ++s )
        {
            if( ast_.kind( statements[s] ) != Node_kind::Fallthrough_stmt )
            {
                continue;
            }

            // Recorded whatever the verdict below: this one has been ruled on here, so the statement walk
            // stays quiet about it either way.
            ruled_fallthroughs_.insert( statements[s].v );

            if( s + 1 != statements.size() )
            {
                reporter_.error_at( ast_.span( statements[s] ), "`fallthrough` must be the last statement of its `case`" );
                continue;
            }

            if( last )
            {
                reporter_.error_at(
                    ast_.span( statements[s] ),
                    "`fallthrough` in the last `case`",
                    "there is no arm after this one to fall into"
                );
                continue;
            }

            // The arm below binds names out of the variant *it* matched. Arriving from here, that
            // variant is not the one in hand, so the names would read fields that were never set.
            if( arm_binds( arms[i + 1] ) )
            {
                reporter_.error_at(
                    ast_.span( statements[s] ),
                    "`fallthrough` into a `case` that destructures",
                    "the next arm binds names from its own variant, and this value is not one"
                );
            }
        }

        // The divergence this whole rule exists for: the same source runs on into the next arm in
        // C, and ends here. So an arm with a body has to say which it means. The last arm is
        // exempt - there is nothing after it either way.
        if( !last && completes_normally( body ) )
        {
            reporter_.error_at(
                ast_.span( arm ),
                "a `case` with a body must say how it ends",
                "end it with `break;` or `return;`, or `fallthrough;` to run on into the next `case`"
            );
        }
    }
}

bool Coverage::completes_normally( Node_id id ) const
{
    switch( ast_.kind( id ) )
    {
    case Node_kind::Return_stmt:
    case Node_kind::Break_stmt:
    case Node_kind::Continue_stmt:
    case Node_kind::Fallthrough_stmt:
        return false;
    case Node_kind::While_stmt:
    case Node_kind::For_stmt:
    case Node_kind::Switch_stmt:
        return true;
    case Node_kind::Block:
    {
        for( const Node_id child : ast_.children( id ) )
        {
            if( !completes_normally( child ) )
            {
                return false;
            }
        }

        return true;
    }
    case Node_kind::If_stmt:
    {
        const std::span<const Node_id> children  = ast_.children( id );
        const Node_id                  otherwise = children[2];

        // Arity is fixed at three and the else *slot* survives when there is no else - so the test
        // is whether that child is valid, never how many there are.
        //
        // With no else the false path falls straight out, so the statement always completes.
        if( !otherwise.is_valid() )
        {
            return true;
        }

        return completes_normally( children[1] ) || completes_normally( otherwise );
    }
    default:
        return true;
    }
}

} // namespace sema
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

// PLAN D7/D30. Exhaustiveness is the whole reason `switch` is worth having over a chain of `if`s:
// adding a variant later breaks exactly the switches that should be revisited. Which is also why a
// `default` on a fully covered enum is refused - it would absorb the new variant silently and take
// the guarantee away.
TEST_CASE( "type_checker_requires_a_switch_to_be_exhaustive", "[sema][switch]" )
{
    constexpr std::string_view colour = "enum Colour { Red, Green, Blue };\n";

    SECTION( "covering every variant is accepted" )
    {
        const Typed p(
            std::string( colour ) + "i32 f( Colour c ) { switch( c ) { case Colour::Red: return 1;"
                                    " case Colour::Green: return 2; case Colour::Blue: return 3; } return 0; }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Named rather than counted: working out *which* is the work the compiler has just done, and
    // "not exhaustive" would hand it back to the author.
    SECTION( "a gap names the variants that are missing" )
    {
        const Typed p(
            std::string( colour ) + "i32 f( Colour c ) { switch( c ) { case Colour::Red: return 1; } return 0; }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not cover every variant of `Colour`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "missing `Green` and `Blue`" ) != std::string::npos );
    }

    SECTION( "and `default` fills it" )
    {
        const Typed p(
            std::string( colour ) + "i32 f( Colour c ) { switch( c ) { case Colour::Red: return 1;"
                                    " default: return 0; } }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "but a `default` that can never run is refused" )
    {
        const Typed p(
            std::string( colour ) + "i32 f( Colour c ) { switch( c ) { case Colour::Red: return 1;"
                                    " case Colour::Green: return 2; case Colour::Blue: return 3;"
                                    " default: return 0; } }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`default` can never run" ) != std::string::npos );
    }
}

TEST_CASE( "type_checker_checks_switch_labels", "[sema][switch]" )
{
    constexpr std::string_view colour = "enum Colour { Red, Green };\n";

    SECTION( "a duplicate label points at the first" )
    {
        const Typed p(
            std::string( colour ) + "i32 f( Colour c ) { switch( c ) { case Colour::Red: return 1;"
                                    " case Colour::Red: return 2; default: return 0; } }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Red` is already covered" ) != std::string::npos );
    }

    SECTION( "a label of the wrong type is refused" )
    {
        const Typed p(
            std::string( colour ) + "i32 f( Colour c ) { switch( c ) { case 1: return 1; default: return 0; } }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "must be a variant of `Colour`" ) != std::string::npos );
    }

    // Stacked labels are one arm, so covering the set across two labels is still exhaustive.
    SECTION( "stacked labels each count towards coverage" )
    {
        const Typed p(
            std::string( colour ) + "i32 f( Colour c ) { switch( c ) { case Colour::Red:"
                                    " case Colour::Green: return 1; } return 0; }\n"
                                    "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// An enum or a number. A `bool` has two values and an `if`, so a switch over one buys nothing and
// would need a `default` or both labels - which is an `if` spelled at length.

// D7. The same source runs on into the next arm in C and ends here, so an arm with a body has to
// say which it means. The last arm is exempt - nothing follows it either way.
TEST_CASE( "type_checker_requires_a_case_to_say_how_it_ends", "[sema][switch][arms]" )
{
    const std::string_view e = "enum E { A, B, C };\n";

    SECTION( "an assignment" )
    {
        const Typed p(
            std::string( e ) + "i32 f( E x ) { i32 t = 0; switch( x ) { case E::A: t = 1; default: t = 2; } return t; }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "must say how it ends" ) != std::string::npos );
    }

    SECTION( "a call" )
    {
        const Typed p(
            std::string( e ) + "void g() { }\n"
                               "void f( E x ) { switch( x ) { case E::A: g(); default: g(); } }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a declaration" )
    {
        const Typed p(
            std::string( e ) + "void f( E x ) { switch( x ) { case E::A: i32 y = 1; default: } }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a `default` in the middle is an arm like any other" )
    {
        const Typed p(
            std::string( e ) + "i32 f( E x ) { i32 t = 0; switch( x ) { default: t = 1; case E::A: t = 2; } return t; }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "every non-final arm is reported, not just the first" )
    {
        const Typed p(
            std::string( e ) +
            "i32 f( E x ) { i32 t = 0; switch( x ) { case E::A: t = 1; case E::B: t = 2; default: t = 3; } return t; }\n"
            "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
    }

    SECTION( "the three fixes all work" )
    {
        for( const char* ending : { "return 1;", "break;", "fallthrough;" } )
        {
            const std::string source = std::string( e ) + "i32 f( E x ) { i32 t = 0; switch( x ) { case E::A: t = 1; " +
                                       ending + " default: t = 2; } return t; }\ni32 main() { return 0; }";
            const Typed p( source );

            INFO( source << "\n" << p.rendered() );
            REQUIRE( p.clean() );
        }
    }

    SECTION( "the last arm needs no ending" )
    {
        const Typed p(
            std::string( e ) + "i32 f( E x ) { i32 t = 0; switch( x ) { case E::A: break; default: t = 2; } return t; }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "stacked labels are one arm, so only the body counts" )
    {
        const Typed p(
            std::string( e ) + "i32 f( E x ) { switch( x ) { case E::A: case E::B: return 1; default: return 2; } }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a numeric switch is held to the same rule" )
    {
        const Typed p( "i32 f( i32 x ) { i32 t = 0; switch( x ) { case 1: t = 1; default: t = 2; } return t; }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// The predicate is "can control reach the end of this arm", not "is the last statement a jump" -
// which is why an `if` that returns on both sides is fine and one that returns on one side is not.

// The predicate is "can control reach the end of this arm", not "is the last statement a jump" -
// which is why an `if` that returns on both sides is fine and one that returns on one side is not.
TEST_CASE( "type_checker_asks_whether_an_arm_can_finish", "[sema][switch][arms]" )
{
    const std::string_view e = "enum E { A, B };\n";

    SECTION( "an if/else where both branches return" )
    {
        const Typed p(
            std::string( e ) +
            "i32 f( E x, bool c ) { switch( x ) { case E::A: if( c ) { return 1; } else { return 2; } default: return 3; } }\n"
            "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an if with no else" )
    {
        const Typed p(
            std::string( e ) + "i32 f( E x, bool c ) { switch( x ) { case E::A: if( c ) { return 1; } default: return 3; } }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "an if/else where only one branch returns" )
    {
        const Typed p(
            std::string( e ) + "i32 f( E x, bool c ) { i32 t = 0; switch( x ) { case E::A: if( c ) { return 1; } else { t = 2; "
                               "} default: return 3; } }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a nested block ending in a jump" )
    {
        const Typed p(
            std::string( e ) + "i32 f( E x ) { switch( x ) { case E::A: { { return 1; } } default: return 3; } }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a loop is assumed to finish" )
    {
        // Conservative: `while( true )` never finishes, and knowing that needs constant folding.
        // Erring this way reports an arm that did not need it, rather than missing one that did.
        const Typed p(
            std::string( e ) + "void f( E x ) { switch( x ) { case E::A: while( true ) { } default: } }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// `break` binds to the nearest enclosing loop *or* switch; `continue` looks past a switch to the
// loop. Two counters rather than one, because the two questions have different answers.

// `fallthrough` is the explicit form of the thing C does implicitly. Three rules keep it readable:
// it ends the arm, it is not in the last one, and it never enters an arm that binds.
TEST_CASE( "type_checker_checks_fallthrough", "[sema][switch][fallthrough]" )
{
    const std::string_view e     = "enum E { A, B, C };\n";
    const std::string_view shape = "enum Shape { Circle( i32 r ), Rect( i32 w, i32 h ) };\n";

    SECTION( "into the next arm" )
    {
        const Typed p(
            std::string( e ) +
            "i32 f( E x ) { i32 t = 0; switch( x ) { case E::A: t = 1; fallthrough; default: t = t + 10; } return t; }\n"
            "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "chained through three arms" )
    {
        const Typed p(
            std::string( e ) + "i32 f( E x ) { i32 t = 0; switch( x ) { case E::A: t = 1; fallthrough; case E::B: t = t + 10; "
                               "fallthrough; default: t = t + 100; } return t; }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "not the last statement of its arm" )
    {
        const Typed p(
            std::string( e ) +
            "i32 f( E x ) { i32 t = 0; switch( x ) { case E::A: fallthrough; t = 1; default: t = 2; } return t; }\n"
            "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "must be the last statement" ) != std::string::npos );
    }

    SECTION( "in the last arm" )
    {
        const Typed p(
            std::string( e ) + "void f( E x ) { switch( x ) { case E::A: break; default: fallthrough; } }\n"
                               "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no arm after this one" ) != std::string::npos );
    }

    SECTION( "into an arm that destructures" )
    {
        // The hazard the whole rule exists for: the next arm names fields out of the variant *it*
        // matched, and arriving from here that is not the variant in hand.
        const Typed p(
            std::string( shape ) +
            "i32 f( Shape s ) { switch( s ) { case Shape::Circle: fallthrough; case Shape::Rect( w, h ): return w; } }\n"
            "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "destructures" ) != std::string::npos );
    }

    SECTION( "into an arm that binds nothing is fine" )
    {
        const Typed p(
            std::string( shape ) + "i32 f( Shape s ) { i32 t = 0; switch( s ) { case Shape::Circle( r ): t = r; fallthrough; "
                                   "case Shape::Rect: t = t + 1; } return t; }\n"
                                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Stacked labels share one body, so they have to agree on what is in scope in it - and the only
// way to agree is to bind nothing. Otherwise a name reads a field the matched variant has not got.
TEST_CASE( "type_checker_refuses_to_stack_a_case_that_destructures", "[sema][switch][fallthrough]" )
{
    const std::string_view shape = "enum Shape { Circle( i32 r ), Rect( i32 w, i32 h ), Dot };\n";

    SECTION( "two labels that both bind" )
    {
        const Typed p(
            std::string( shape ) + "i32 f( Shape s ) { switch( s ) { case Shape::Circle( r ): case Shape::Rect( w, h ): return "
                                   "1; default: return 0; } }\n"
                                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot share its body" ) != std::string::npos );
    }

    SECTION( "one that binds stacked with one that does not" )
    {
        const Typed p(
            std::string( shape ) +
            "i32 f( Shape s ) { switch( s ) { case Shape::Circle( r ): case Shape::Dot: return 1; default: return 0; } }\n"
            "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "two bare labels over payload variants" )
    {
        const Typed p(
            std::string( shape ) +
            "i32 f( Shape s ) { switch( s ) { case Shape::Circle: case Shape::Rect: return 1; default: return 0; } }\n"
            "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a single destructuring label is untouched" )
    {
        const Typed p(
            std::string( shape ) +
            "i32 f( Shape s ) { switch( s ) { case Shape::Circle( r ): return r; default: return 0; } }\n"
            "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A bare label names the variant and ignores its payload, which is a complete thing to say in a
// `case` even when the variant carries one. Naming a variant and producing one are different, and
// only the second needs the payload.

// A bare label names the variant and ignores its payload, which is a complete thing to say in a
// `case` even when the variant carries one. Naming a variant and producing one are different, and
// only the second needs the payload.
TEST_CASE( "type_checker_matches_a_variant_without_destructuring_it", "[sema][switch][fallthrough]" )
{
    const std::string_view shape = "enum Shape { Circle( i32 r ), Rect( i32 w, i32 h ) };\n";

    SECTION( "a bare label covers its variant" )
    {
        const Typed p(
            std::string( shape ) +
            "i32 f( Shape s ) { switch( s ) { case Shape::Circle: return 1; case Shape::Rect: return 2; } }\n"
            "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "mixed with a destructuring arm" )
    {
        const Typed p(
            std::string( shape ) +
            "i32 f( Shape s ) { switch( s ) { case Shape::Circle: return 1; case Shape::Rect( w, h ): return w; } }\n"
            "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a missing variant is still missing" )
    {
        const Typed p(
            std::string( shape ) + "i32 f( Shape s ) { switch( s ) { case Shape::Circle: return 1; } }\n"
                                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "does not cover every variant" ) != std::string::npos );
    }

    SECTION( "a duplicate is still a duplicate, whichever form each uses" )
    {
        const Typed p(
            std::string( shape ) + "i32 f( Shape s ) { switch( s ) { case Shape::Circle: return 1; case Shape::Circle( r ): "
                                   "return r; case Shape::Rect: return 2; } }\n"
                                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "already covered" ) != std::string::npos );
    }

    SECTION( "but a bare path is still not a value" )
    {
        // The half of the rule that stays: `Shape::Circle` names a variant here and produces one
        // nowhere, so expression position is unchanged.
        const Typed p( std::string( shape ) + "i32 main() { Shape s = Shape::Circle; return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "carries a payload" ) != std::string::npos );
    }
}

// The call site's half of slice 1b. Reported before the arguments are matched against a signature,
// because with nothing instantiated there is no signature to match them against - checking them
// against the uninstantiated one reports about a call the author did not write.

// The call site's half of slice 1b. Reported before the arguments are matched against a signature,
// because with nothing instantiated there is no signature to match them against - checking them
// against the uninstantiated one reports about a call the author did not write.
TEST_CASE( "type_checker_checks_a_numeric_switch", "[sema][range]" )
{
    SECTION( "an integer scrutinee is accepted with a default" )
    {
        const Typed p( "i32 f( i32 n ) { switch( n ) { case 1: return 1; default: return 0; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Completeness over 2^32 values is not provable, so `default` is required rather than inferred -
    // which is the one place a numeric switch differs from an enum one in kind rather than degree.
    SECTION( "and required to have one" )
    {
        const Typed p( "i32 f( i32 n ) { switch( n ) { case 1: return 1; } return 0; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "needs a `default`" ) != std::string::npos );
    }

    // And the dead-`default` rule deliberately does *not* apply: nothing can prove the intervals
    // cover the type, so no `default` over a number is ever provably unreachable.
    SECTION( "a default is never dead over a number" )
    {
        const Typed p( "i32 f( i32 n ) { switch( n ) { case 0..256: return 1; default: return 0; } }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

TEST_CASE( "type_checker_checks_range_labels", "[sema][range]" )
{
    SECTION( "a backwards range is refused" )
    {
        const Typed p( "i32 f( i32 n ) { switch( n ) { case 5..1: return 1; default: return 0; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`5..1` matches nothing" ) != std::string::npos );
    }

    // Half-open, so equal bounds exclude everything. Its own message, because the fix differs: this
    // one is off by one rather than the wrong way round.
    SECTION( "and so is an empty one" )
    {
        const Typed p( "i32 f( i32 n ) { switch( n ) { case 3..3: return 1; default: return 0; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "excludes its upper bound" ) != std::string::npos );
    }

    SECTION( "an overlap names the earlier label" )
    {
        const Typed p( "i32 f( i32 n ) { switch( n ) { case 1..10: return 1; case 5: return 2; default: return 0; } }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "overlaps an earlier one" ) != std::string::npos );
    }

    // Half-open is what makes these two *not* overlap, and it is the boundary an off-by-one would
    // land on - so it is worth a case of its own rather than trusting the one above.
    SECTION( "but touching ranges do not overlap" )
    {
        const Typed p( "i32 f( i32 n ) { switch( n ) { case 1..5: return 1; case 5..10: return 2; default: return 0; } }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a label must be a constant" )
    {
        const Typed p( "i32 f( i32 n, i32 m ) { switch( n ) { case m: return 1; default: return 0; } }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "known at compile time" ) != std::string::npos );
    }

    // D34 gives `..` one meaning everywhere, and the meaning slicing will need is an integer one.
    // A range of variants would additionally need declaration order to be an ordering, which enum
    // comparison already refuses to treat it as.
    SECTION( "a range cannot name variants" )
    {
        const Typed p( "enum Colour { Red, Green };\n"
                       "i32 f( Colour c ) { switch( c ) { case Colour::Red..Colour::Green: return 1; default: return 0; } }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "a range cannot name variants" ) != std::string::npos );
    }
}

// A float scrutinee takes ranges and refuses single values. Exact equality against a float is the
// mistake every float guide opens with, and a `case` is the one place it would look deliberate;
// a range says what was meant. The bounds stay integers either way - `..` has one meaning.

// A float scrutinee takes ranges and refuses single values. Exact equality against a float is the
// mistake every float guide opens with, and a `case` is the one place it would look deliberate;
// a range says what was meant. The bounds stay integers either way - `..` has one meaning.
// A single `case` label is the one-wide interval it covers, which is what lets one overlap walk
// serve both forms. Both halves of that are load-bearing: the width catches a repeat, and the
// pairwise test is symmetric so source order carries no meaning.
TEST_CASE( "coverage_reports_a_repeated_numeric_label", "[sema][range]" )
{
    const Typed p( "i32 f( i32 n ) { switch( n ) { case 1: return 1; case 1: return 2; default: return 0; } }\n"
                   "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "overlaps an earlier one" ) != std::string::npos );
}

TEST_CASE( "coverage_accepts_numeric_labels_written_out_of_order", "[sema][range]" )
{
    const Typed p( "i32 f( i32 n ) { switch( n ) { case 5: return 1; case 1..3: return 2; default: return 0; } }\n"
                   "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

TEST_CASE( "type_checker_switches_on_a_float_by_range_only", "[sema][range]" )
{
    SECTION( "a range bucket is accepted" )
    {
        const Typed p( "i32 f( f64 x ) { switch( x ) { case 0..1: return 1; case 1..10: return 2; default: return 3; } }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a single value is refused" )
    {
        const Typed p( "i32 f( f64 x ) { switch( x ) { case 3: return 1; default: return 0; } }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "a single value cannot be matched against a `f64`" ) != std::string::npos );
    }
}

// A pattern binds the payload by name. The binding is read-only: when owning payloads arrive it
// becomes a borrow, and writing through it would then be writing into a value the enum still owns.

// A pattern binds the payload by name. The binding is read-only: when owning payloads arrive it
// becomes a borrow, and writing through it would then be writing into a value the enum still owns.
TEST_CASE( "type_checker_binds_a_variant_pattern", "[sema][payload]" )
{
    constexpr std::string_view shape = "enum Shape { Circle( f64 radius ), Rect( f64 w, f64 h ), Dot };\n";

    SECTION( "the binding has the payload's type" )
    {
        const Typed p(
            std::string( shape ) + "f64 area( Shape s ) { switch( s ) {"
                                   " case Shape::Circle( r ): return r * r;"
                                   " case Shape::Rect( w, h ): return w * h;"
                                   " case Shape::Dot: return 0.0; } }\n"
                                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "the wrong number of bindings is refused" )
    {
        const Typed p(
            std::string( shape ) + "f64 area( Shape s ) { switch( s ) {"
                                   " case Shape::Rect( w ): return w;"
                                   " default: return 0.0; } }\n"
                                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "a pattern on a payload-free variant is refused" )
    {
        const Typed p(
            std::string( shape ) + "f64 area( Shape s ) { switch( s ) {"
                                   " case Shape::Dot( x ): return x;"
                                   " default: return 0.0; } }\n"
                                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // The binding is scoped to its arm, which is what stops one arm reading another's payload.
    SECTION( "a binding does not escape its arm" )
    {
        const Typed p(
            std::string( shape ) + "f64 area( Shape s ) { switch( s ) {"
                                   " case Shape::Circle( r ): return 0.0;"
                                   " default: return r; } }\n"
                                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`r` is not declared" ) != std::string::npos );
    }

    SECTION( "and it is read-only" )
    {
        const Typed p(
            std::string( shape ) + "f64 area( Shape s ) { switch( s ) {"
                                   " case Shape::Circle( r ): r = 2.0; return r;"
                                   " default: return 0.0; } }\n"
                                   "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

// Exhaustiveness does not change: a payload variant is covered by naming it, with or without a
// pattern, and the missing-variant message names it the same way.
TEST_CASE( "type_checker_counts_payload_variants_for_exhaustiveness", "[sema][payload]" )
{
    const Typed p( "enum Shape { Circle( f64 radius ), Dot };\n"
                   "f64 area( Shape s ) { switch( s ) { case Shape::Dot: return 0.0; } }\n"
                   "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.rendered().find( "missing `Circle`" ) != std::string::npos );
}

} // namespace keel
#endif
