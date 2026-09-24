#include "sema/bounds.h"
#include <fmt/format.h>

namespace keel
{
namespace
{

// D40's set is closed, so a table rather than a lookup structure: six entries, scanned once per
// bound written. Also the one place that knows the spellings, which is what lets a diagnostic list
// them all when a name is not one of them.
struct Named_bound
{
    std::string_view name;
    sema::Bound      bound;
};

constexpr Named_bound k_bounds[] = {
    { "Copyable", sema::Bound::Copyable },
    { "Equatable", sema::Bound::Equatable },
    { "Comparable", sema::Bound::Comparable },
    { "Numeric", sema::Bound::Numeric },
    { "Integral", sema::Bound::Integral },
    { "Floating", sema::Bound::Floating },
};

// Absent pairs are refused, and the omissions are the point: float to int has no one obvious
// rounding, int to bool says less than `x != 0` does, and nothing converts to or from a struct.
struct Conversion_rule
{
    Type_kind        from;
    Type_kind        to;
    sema::Conversion allows;
};

constexpr Conversion_rule conversion_rules[] = {
    { Type_kind::Int, Type_kind::Int, sema::Conversion::Both },
    { Type_kind::Int, Type_kind::Float, sema::Conversion::Cast_only },
    { Type_kind::Float, Type_kind::Float, sema::Conversion::Cast_only },
    { Type_kind::Bool, Type_kind::Int, sema::Conversion::Cast_only },
    { Type_kind::Pointer, Type_kind::Pointer, sema::Conversion::Unsafe },
};

sema::Conversion conversion_for( Type_kind from, Type_kind to )
{
    for( const Conversion_rule& rule : conversion_rules )
    {
        if( rule.from == from && rule.to == to )
        {
            return rule.allows;
        }
    }

    return sema::Conversion::None;
}

} // namespace

namespace sema
{
// "Copyable, Equatable, ... and Floating", from the table above rather than from a string - so a
// seventh bound updates every message that lists them without anyone remembering to.
std::string known_bound_names()
{
    std::string listed;

    for( std::size_t i = 0; i < std::size( k_bounds ); ++i )
    {
        if( i != 0 )
        {
            listed += i + 1 == std::size( k_bounds ) ? " and " : ", ";
        }

        listed += k_bounds[i].name;
    }

    return listed;
}

std::string_view name_of_bound( Bound bound )
{
    for( const Named_bound& known : k_bounds )
    {
        if( known.bound == bound )
        {
            return known.name;
        }
    }

    return "?";
}

// Phrased as the requirement rather than as the failure: the error already says which bound was not
// met, and what the author needs next is what would meet it.
std::string_view bound_requirement( Bound bound )
{
    switch( bound )
    {
    case Bound::Copyable:
        return "a type with a destructor owns something, so copying it would own it twice";
    case Bound::Equatable:
        return "equality needs a number, a `bool`, an `enum` or a pointer";
    case Bound::Comparable:
        return "ordering needs a number";
    case Bound::Numeric:
        return "it has to be an integer or a float";
    case Bound::Integral:
        return "it has to be an integer";
    case Bound::Floating:
        return "it has to be a float";
    }

    return {};
}

std::optional<Bound> bound_for_name( std::string_view spelling )
{
    for( const Named_bound& known : k_bounds )
    {
        if( known.name == spelling )
        {
            return known.bound;
        }
    }

    return std::nullopt;
}

// Nothing for `&&` and `||`: no bound in D40's set makes a `T` a bool, so they are left to the rule
// table, which already rejects them with what the operands have to be.
std::optional<Bound> operator_to_bound( Token_kind kind )
{
    switch( kind )
    {
    case Token_kind::Equal_equal:
    case Token_kind::Bang_equal:
        return Bound::Equatable;
    case Token_kind::Less:
    case Token_kind::Greater:
    case Token_kind::Less_equal:
    case Token_kind::Greater_equal:
        return Bound::Comparable;
    case Token_kind::Plus:
    case Token_kind::Minus:
    case Token_kind::Star:
    case Token_kind::Slash:
        return Bound::Numeric;
    case Token_kind::Percent:
    case Token_kind::Amp:
    case Token_kind::Pipe:
    case Token_kind::Caret:
    case Token_kind::Less_less:
    case Token_kind::Greater_greater:
        return Bound::Integral;

    default:
        return std::nullopt;
    }
}

// A bound is a promise about a *parameter*, so only a parameter can carry one. Anything else, and
// an unbounded parameter, comes back as the empty set - which promises nothing, and is why `id` is
// close to the only generic writable without a `where` clause. Concrete types go to satisfies().
bool Bounds::has_bound( Type_id type, Bound bound ) const
{
    return contains( bounds_for( type ), bound );
}

// has_bound asks what a parameter *promises*; this asks what a concrete type *delivers*. Forwarding
// a parameter back to has_bound is not a special case but the feature: it is what makes a generic
// calling a generic legal exactly when the inner bounds are a subset of the outer ones.
bool Bounds::satisfies( Type_id type, Bound bound ) const
{
    if( !type.is_valid() || table_.is_error( type ) )
    {
        return true; // already reported; every bound would fail on it and say nothing new
    }

    if( table_.is_parameter( type ) )
    {
        return has_bound( type, bound );
    }

    switch( bound )
    {
    case Bound::Copyable:
        return !aggregates_.owns( type );

    // Ordering is deliberately narrower than equality: a pointer and an enum can be told apart but
    // cannot be ranked.
    case Bound::Equatable:
        return table_.is_integer( type ) || table_.is_float( type ) || type == table_.builtin( Type_kind::Bool ) ||
               table_.is_enum( type ) || table_.is_pointer( type );
    case Bound::Comparable:
    case Bound::Numeric:
        return table_.is_integer( type ) || table_.is_float( type );
    case Bound::Integral:
        return table_.is_integer( type );
    case Bound::Floating:
        return table_.is_float( type );
    }

    return false;
}

// Reports every unmet bound rather than the first: two are two things to fix, and one per compile is
// two compiles. What it does *not* report twice is one mistake wearing several names - closure()
// stored `Integral` as `Integral | Numeric | Comparable | Equatable`, so a struct fails all four.
//
// A span rather than a node, because a deduced type argument has no annotation to underline.
void Bounds::check_bounds( Node_id parameter, Span at, Type_id argument, std::string_view callee )
{
    const auto found = bounds_.find( parameter.v );

    if( found == bounds_.end() )
    {
        return; // unbounded: nothing promised, so nothing to check
    }

    Bound_set failing = 0;

    for( const Named_bound& known : k_bounds )
    {
        if( contains( found->second, known.bound ) && !satisfies( argument, known.bound ) )
        {
            failing |= static_cast<Bound_set>( known.bound );
        }
    }

    // A failing bound that another failing bound implies is the same mistake said again. Computed
    // from closure() rather than from the order of k_bounds, so a seventh bound needs nothing here.
    Bound_set implied = 0;

    for( const Named_bound& known : k_bounds )
    {
        if( contains( failing, known.bound ) )
        {
            implied |= closure( static_cast<Bound_set>( known.bound ) ) & ~static_cast<Bound_set>( known.bound );
        }
    }

    for( const Named_bound& known : k_bounds )
    {
        if( !contains( failing & ~implied, known.bound ) )
        {
            continue;
        }

        // A parameter as the type argument is a generic forwarding its own `T`, and the fix is one
        // clause away rather than one type away. "ordering needs a number" would be the wrong advice.
        const bool forwarded = table_.is_parameter( argument );

        std::string help = std::string( bound_requirement( known.bound ) );

        if( forwarded )
        {
            // The declaration to name is the one that introduced *the argument's* parameter: for a
            // field of a generic aggregate that is the aggregate, and reading the enclosing function
            // instead aborted on every annotation the signature pass reached.
            const Node_id owner = owner_of_.at( table_.get( argument ).declaration.v );

            help = fmt::format(
                "add `{}` to the `where` clause for `{}` on `{}`",
                known.name,
                table_.name( argument ),
                interner_.text( Symbol_id { ast_.aux( owner ) } )
            );
        }

        reporter_.error_at(
            at,
            fmt::format(
                "`{}` {} `{}`, and `{}` requires it of `{}`",
                table_.name( argument ),
                forwarded ? "does not promise" : "is not",
                known.name,
                callee,
                interner_.text( Symbol_id { ast_.aux( parameter ) } )
            ),
            std::move( help )
        );
    }
}

Bound_set Bounds::bounds_for( Type_id parameter ) const
{
    if( !table_.is_parameter( parameter ) )
    {
        return 0;
    }

    const Node_id declaration = table_.get( parameter ).declaration;
    const auto    found       = declaration.is_valid() ? bounds_.find( declaration.v ) : bounds_.end();

    // An unbounded parameter has no entry rather than an empty one, and promises nothing.
    return found == bounds_.end() ? Bound_set { 0 } : found->second;
}

// The same question a literal's range asks, for a width rather than a value: a shift count has to
// be in range for every type `T` may be, and the narrowest admissible integer is what decides it.
u8 Bounds::narrowest_admissible_width( Type_id parameter ) const
{
    u8 narrowest = 0;

    for( const Type_id candidate : admissible_numeric_types( bounds_for( parameter ) ) )
    {
        if( !table_.is_integer( candidate ) )
        {
            continue;
        }

        const u8 width = table_.get( candidate ).width;

        if( narrowest == 0 || width < narrowest )
        {
            narrowest = width;
        }
    }

    return narrowest;
}

// A conversion on a `T` is legal when it is legal for *every* type `T` may turn out to be, and means
// what the weakest of them means. The answer is the same at every instantiation, which is what keeps
// D11 whole: no later call site can break a body that compiled.
Conversion Bounds::conversion_between( Type_id from, Type_id to, Type_id& witness ) const
{
    witness = Type_id {};

    // Neither side generic: the pair table is the whole answer, `Unsafe` included. Below it cannot
    // arise, because no bound admits a pointer.
    if( !table_.is_parameter( from ) && !table_.is_parameter( to ) )
    {
        return conversion_for( table_.get( from ).kind, table_.get( to ).kind );
    }

    const std::vector<Type_id> froms = possible_types( from );
    const std::vector<Type_id> tos   = possible_types( to );

    // A side that is not enumerable has no set to check against, and a vacuous product answers
    // `Both` - which would let `cast<f64>` through on a `Copyable` `T` that may be a struct. The
    // caller reports the missing bound; refusing here is what stops the hole existing at all.
    if( froms.empty() || tos.empty() )
    {
        return Conversion::None;
    }

    Conversion weakest = Conversion::Both;

    for( const Type_id f : froms )
    {
        for( const Type_id t : tos )
        {
            const Conversion pair = conversion_for( table_.get( f ).kind, table_.get( t ).kind );

            assert( pair != Conversion::Unsafe && "no bound admits a pointer, so no pair here is one" );

            // Nothing is weaker than a refusal, so no later pair can change the answer.
            if( pair == Conversion::None )
            {
                witness = table_.is_parameter( from ) ? f : t;

                return Conversion::None;
            }

            // The earliest witness is the narrowest admissible type, which is the one worth naming.
            if( pair == Conversion::Cast_only && weakest == Conversion::Both )
            {
                weakest = Conversion::Cast_only;
                witness = table_.is_parameter( from ) ? f : t;
            }
        }
    }

    return weakest;
}

// The narrowing half of the same product: a `cast` is safe only when *every* admissible pair widens,
// and one that narrows is the whole answer.
Type_id Bounds::narrowing_witness( Type_id from, Type_id to ) const
{
    if( !table_.is_parameter( from ) && !table_.is_parameter( to ) )
    {
        return table_.holds( from, to ) ? Type_id {} : from;
    }

    const std::vector<Type_id> froms = possible_types( from );
    const std::vector<Type_id> tos   = possible_types( to );

    // Unreachable: conversion_between refused an unenumerable side before this arm was taken. The
    // guard is here because a vacuous product would answer "nothing narrows", which is backwards.
    if( froms.empty() || tos.empty() )
    {
        return from;
    }

    for( const Type_id f : froms )
    {
        for( const Type_id t : tos )
        {
            if( !table_.holds( f, t ) )
            {
                return table_.is_parameter( from ) ? f : t;
            }
        }
    }

    return Type_id {};
}

Type_id Bounds::type_the_literal_overflows( Node_id literal, bool negative, Bound_set bounds ) const
{
    const Literal_id value { ast_.aux( literal ) };

    // A literal the lexer could not scan has no value recorded. It reported there.
    if( !value.is_valid() )
    {
        return Type_id {};
    }

    // The *literal's* kind decides which pool the value lives in, never the candidate's: asking
    // Literal_pool for a float it never stored reads past the end of the wrong vector.
    const bool floating_literal = ast_.kind( literal ) == Node_kind::Float_literal;

    for( const Type_id candidate : admissible_numeric_types( bounds ) )
    {
        if( floating_literal )
        {
            // A float literal only reaches here under `Floating`, whose admissible set is floats.
            assert( table_.is_float( candidate ) && "a fractional literal was measured against an integer" );

            if( !table_.fits_float( literal_pool_.floating( value ), candidate ) )
            {
                return candidate;
            }

            continue;
        }

        const u64 magnitude = literal_pool_.integer( value );

        const bool holds =
            table_.is_float( candidate )
                ? table_.fits_float( negative ? -static_cast<f64>( magnitude ) : static_cast<f64>( magnitude ), candidate )
                : table_.fits( magnitude, negative, candidate );

        if( !holds )
        {
            return candidate;
        }
    }

    return Type_id {};
}

// Shared between a generic function and a generic aggregate, which want the same thing and differ
// only in which child holds the list. A non-generic declaration has no list at all, which is the
// ordinary case rather than an absence worth testing for at every call.
//
// Must run before the signature or the fields are typed: `T` then resolves through the ordinary
// annotation path, and `satisfies` can already answer for it. The pair of `out T` tests holds that
// order.
void Bounds::declare_type_parameters( Node_id declaration, Node_id list )
{
    if( !list.is_valid() )
    {
        return;
    }

    const std::span<const Node_id> type_param_list = ast_.children( list );

    for( const Node_id type_param : type_param_list )
    {
        if( ast_.kind( type_param ) != Node_kind::Type_param_decl )
        {
            continue;
        }

        types_.record( type_param, table_.parameter( type_param, interner_.text( Symbol_id { ast_.aux( type_param ) } ) ) );
        owner_of_.emplace( type_param.v, declaration );
    }

    for( const Node_id where_clause : type_param_list )
    {
        if( ast_.kind( where_clause ) != Node_kind::Where_clause )
        {
            continue;
        }

        const Symbol_id subject_name = Symbol_id { ast_.aux( where_clause ) };

        const auto it = std::find_if(
            type_param_list.begin(),
            type_param_list.end(),
            [&]( const Node_id type_param ) {
                return ast_.kind( type_param ) == Node_kind::Type_param_decl &&
                       Symbol_id { ast_.aux( type_param ) } == subject_name;
            }
        );

        if( it == type_param_list.end() )
        {
            reporter_.error_at(
                ast_.span( where_clause ),
                fmt::format(
                    "`{}` is not a type parameter of `{}`",
                    interner_.text( subject_name ),
                    interner_.text( Symbol_id { ast_.aux( declaration ) } )
                )
            );
            continue;
        }

        // Keyed by the Type_param_decl node, never by its name: two declarations may each
        // have a parameter called `T`, and keying by name would merge their bounds.
        if( bounds_.contains( it->v ) )
        {
            reporter_.error_at(
                ast_.span( where_clause ),
                fmt::format( "duplicate where clause for type parameter `{}`", interner_.text( subject_name ) )
            );
            continue;
        }

        Bound_set direct_bounds = 0;
        for( const Node_id bound_node : ast_.children( where_clause ) )
        {
            std::string_view     bound_name = interner_.text( Symbol_id { ast_.aux( bound_node ) } );
            std::optional<Bound> bound      = bound_for_name( bound_name );

            if( !bound )
            {
                // The span is the name, not the clause, and the list comes from the table
                // rather than from this string - so adding a bound updates the message.
                reporter_.error_at(
                    ast_.span( bound_node ),
                    fmt::format( "unknown bound `{}`", bound_name ),
                    fmt::format( "the bounds are {}", known_bound_names() )
                );
                continue;
            }

            direct_bounds = closure( direct_bounds | static_cast<Bound_set>( bound.value() ) );
        }

        bounds_.emplace( it->v, direct_bounds );
    }
}

Bound_set Bounds::closure( Bound_set direct )
{
    if( contains( direct, Bound::Integral ) || contains( direct, Bound::Floating ) )
    {
        direct |= Bound::Numeric;
    }

    if( contains( direct, Bound::Numeric ) )
    {
        direct |= Bound::Comparable;

        // The one implication that is a fact about *satisfaction* rather than about the bounds:
        // D33's operators are not shipped, so only a builtin can satisfy `Numeric` today and every
        // builtin copies. An arithmetic class that owns a resource would make it false, and the
        // failure is safe - `Copyable` is in the promised set, so the call site refuses it.
        //
        // Deliberately not extended to `Comparable` and `Equatable`: a string orders and compares
        // and owns its bytes, and keeping those two borrowing is what leaves room for it.
        direct |= Bound::Copyable;
    }

    if( contains( direct, Bound::Comparable ) )
    {
        direct |= Bound::Equatable;
    }

    return direct;
}

// Every concrete type a value may turn out to be. A parameter without `Numeric` comes back empty and
// the emptiness is the point: `Copyable` alone admits every struct, which cannot be enumerated. An
// empty answer means "not enumerable", never "nothing to check".
std::vector<Type_id> Bounds::possible_types( Type_id type ) const
{
    if( !table_.is_parameter( type ) )
    {
        return { type };
    }

    const Bound_set bounds = bounds_for( type );

    // `Integral` and `Floating` are stored with `Numeric` in them by closure(), so this one test
    // covers all three.
    return contains( bounds, Bound::Numeric ) ? admissible_numeric_types( bounds ) : std::vector<Type_id> {};
}

std::vector<Type_id> Bounds::admissible_numeric_types( Bound_set bounds ) const
{
    std::vector<Type_id> result;

    const Type_id types[] = {
        table_.integer( 8, true ),
        table_.integer( 8, false ),
        table_.integer( 16, true ),
        table_.integer( 16, false ),
        table_.integer( 32, true ),
        table_.integer( 32, false ),
        table_.integer( 64, true ),
        table_.integer( 64, false ),
        table_.floating( 32 ),
        table_.floating( 64 ),
    };
    for( const Type_id type : types )
    {
        bool satisfies_all = true;
        for( const Named_bound& known : k_bounds )
        {
            if( contains( bounds, known.bound ) && !satisfies( type, known.bound ) )
            {
                satisfies_all = false;
                break;
            }
        }
        if( satisfies_all )
        {
            result.push_back( type );
        }
    }

    return result;
}

} // namespace sema
} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <string_view>

#include "common/source_manager.h"
#include "lex/lexer.h"
#include "parse/parser.h"
#include "sema/checker_test_support.h"

namespace keel
{
namespace
{

// The fixture and the cases below name the vocabulary bounds.h declares in `keel::sema`.
using namespace sema;

// Bounds needs a parsed `where` clause and a type table, and nothing else. The fixture supplies
// that: it interns one struct type per aggregate, declares the type parameters, and settles the
// owning set so the `Copyable` arm has an answer. No resolver, no checker, no expression walk.
class Promises
{
public:
    explicit Promises( std::string_view source )
        : ast_(
              parse( lex( sm_.add_file( "t.kl", std::string( source ) ), sm_, interner_, literal_pool_, diags_ ), sm_, diags_ )
          ),
          reporter_( sm_, diags_ ),
          aggregates_( ast_, interner_, types_, reporter_ ),
          bounds_( ast_, interner_, literal_pool_, types_, aggregates_, reporter_ )
    {
        types_.size_to( ast_.node_count() );

        for( const Node_id decl : ast_.children( ast_.root() ) )
        {
            bounds_.declare_type_parameters( decl, ast_.type_param_list( decl ) );

            if( !is_aggregate( ast_.kind( decl ) ) )
            {
                continue;
            }

            types_.table().structure( decl, {}, interner_.text( Symbol_id { ast_.aux( decl ) } ) );

            for( const Node_id field : ast_.members( decl ) )
            {
                if( ast_.kind( field ) != Node_kind::Field_decl )
                {
                    continue;
                }

                const Node_id annotation = ast_.child( field, 0 );

                if( ast_.kind( annotation ) != Node_kind::Named_type )
                {
                    continue;
                }

                types_.record( field, types_.table().from_spelling( interner_.text( Symbol_id { ast_.aux( annotation ) } ) ) );
            }
        }

        aggregates_.order_structs();
        aggregates_.compute_owning();
    }

    sema::Bounds& bounds()
    {
        return bounds_;
    }

    Type_table& table()
    {
        return types_.table();
    }

    // The nth Type_param_decl in source order, over every declaration that has a list.
    Node_id parameter_decl( std::size_t index ) const
    {
        for( const Node_id decl : ast_.children( ast_.root() ) )
        {
            const Node_id list = ast_.type_param_list( decl );

            if( !list.is_valid() )
            {
                continue;
            }

            for( const Node_id type_param : ast_.children( list ) )
            {
                if( ast_.kind( type_param ) == Node_kind::Type_param_decl && index-- == 0 )
                {
                    return type_param;
                }
            }
        }

        return Node_id {};
    }

    Type_id parameter( std::size_t index ) const
    {
        return types_.type_of( parameter_decl( index ) );
    }

    Type_id aggregate( std::size_t index )
    {
        for( const Node_id decl : ast_.children( ast_.root() ) )
        {
            if( is_aggregate( ast_.kind( decl ) ) && index-- == 0 )
            {
                return types_.table().structure( decl, {}, interner_.text( Symbol_id { ast_.aux( decl ) } ) );
            }
        }

        return Type_id {};
    }

    // The nth literal of a kind, for the range question type_the_literal_overflows answers.
    Node_id literal( Node_kind kind, std::size_t index ) const
    {
        for( u32 i = 0; i < ast_.node_count(); ++i )
        {
            if( ast_.kind( Node_id { i } ) == kind && index-- == 0 )
            {
                return Node_id { i };
            }
        }

        return Node_id {};
    }

    Span span( Node_id id ) const
    {
        return ast_.span( id );
    }

    std::size_t errors() const
    {
        return diags_.error_count();
    }

    std::string rendered() const
    {
        std::ostringstream out;
        diags_.render( sm_, out );
        return out.str();
    }

private:
    Source_manager sm_;
    Interner       interner_;
    Literal_pool   literal_pool_;
    Diagnostics    diags_;
    Ast            ast_;

    sema::Types_builder types_;
    sema::Reporter      reporter_;
    sema::Aggregates    aggregates_;
    sema::Bounds        bounds_;
};

TEST_CASE( "bounds_close_a_where_clause_over_what_it_implies", "[sema][bound]" )
{
    Promises p( "void f<T>( const ref T a ) where T : Integral { }\n"
                "void g<U>( const ref U a ) where U : Comparable { }\n" );

    const Bound_set integral = p.bounds().bounds_for( p.parameter( 0 ) );

    REQUIRE( contains( integral, Bound::Integral ) );
    REQUIRE( contains( integral, Bound::Numeric ) );
    REQUIRE( contains( integral, Bound::Comparable ) );
    REQUIRE( contains( integral, Bound::Equatable ) );
    REQUIRE( contains( integral, Bound::Copyable ) );

    // Downhill only. Ordering implies equality and nothing else - `Copyable` in particular is a
    // fact about numbers, and a type that orders may still own its bytes.
    const Bound_set comparable = p.bounds().bounds_for( p.parameter( 1 ) );

    REQUIRE( contains( comparable, Bound::Equatable ) );
    REQUIRE( !contains( comparable, Bound::Numeric ) );
    REQUIRE( !contains( comparable, Bound::Copyable ) );
    REQUIRE( !contains( comparable, Bound::Integral ) );
}

TEST_CASE( "bounds_ask_a_parameter_what_it_promises_and_a_type_what_it_delivers", "[sema][bound]" )
{
    Promises p( "void f<T>( const ref T a ) where T : Integral { }\n" );

    const Type_id parameter = p.parameter( 0 );

    REQUIRE( p.bounds().has_bound( parameter, Bound::Numeric ) );
    REQUIRE( !p.bounds().has_bound( parameter, Bound::Floating ) );
    REQUIRE( p.bounds().satisfies( parameter, Bound::Numeric ) ); // forwarded, which is what lets a generic call a generic
    REQUIRE( !p.bounds().satisfies( parameter, Bound::Floating ) );

    // A concrete type promises nothing and delivers what it is.
    REQUIRE( !p.bounds().has_bound( p.table().integer( 32, true ), Bound::Integral ) );
    REQUIRE( p.bounds().satisfies( p.table().integer( 32, true ), Bound::Integral ) );
    REQUIRE( !p.bounds().satisfies( p.table().floating( 64 ), Bound::Integral ) );
    REQUIRE( p.bounds().satisfies( p.table().floating( 64 ), Bound::Numeric ) );

    // An error type meets everything: the mistake was reported, and no bound would say more.
    REQUIRE( p.bounds().satisfies( Type_id {}, Bound::Integral ) );
    REQUIRE( p.bounds().satisfies( p.table().builtin( Type_kind::Error ), Bound::Integral ) );
}

TEST_CASE( "bounds_take_copyable_from_the_structural_answer", "[sema][bound][owning]" )
{
    Promises p( "class Res { i32 h; ~Res() { } };\nstruct Plain { i32 x; };\n" );

    REQUIRE( !p.bounds().satisfies( p.aggregate( 0 ), Bound::Copyable ) );
    REQUIRE( p.bounds().satisfies( p.aggregate( 1 ), Bound::Copyable ) );
    REQUIRE( p.bounds().satisfies( p.table().integer( 32, true ), Bound::Copyable ) );
}

TEST_CASE( "bounds_refuse_a_conversion_on_a_parameter_they_cannot_enumerate", "[sema][bound]" )
{
    Promises p( "void f<T>( const ref T a ) where T : Numeric { }\n"
                "void g<U>( const ref U a ) where U : Copyable { }\n" );

    Type_id witness;

    REQUIRE(
        p.bounds().conversion_between( p.table().integer( 32, true ), p.table().integer( 64, true ), witness ) ==
        Conversion::Both
    );
    REQUIRE( p.bounds().conversion_between( p.parameter( 0 ), p.table().floating( 64 ), witness ) == Conversion::Cast_only );
    REQUIRE( witness.is_valid() ); // an admissible type decided it, and that is what the help names

    // `Copyable` admits every struct, which admissible_numeric_types cannot enumerate - so the
    // product is empty, and an empty product is a refusal rather than a vacuous yes.
    REQUIRE( p.bounds().conversion_between( p.parameter( 1 ), p.table().floating( 64 ), witness ) == Conversion::None );

    // No admissible type decided it because there were none, so the help asks for the bound
    // instead of naming a type. A witness here would be the parameter explaining itself.
    REQUIRE( !witness.is_valid() );
}

TEST_CASE( "bounds_name_an_admissible_type_rather_than_the_parameter", "[sema][bound]" )
{
    Promises p( "void f<T>( const ref T a ) where T : Numeric { }\n" );

    REQUIRE( !p.bounds().narrowing_witness( p.table().integer( 32, true ), p.table().integer( 64, true ) ).is_valid() );
    REQUIRE(
        p.bounds().narrowing_witness( p.table().integer( 64, true ), p.table().integer( 32, true ) ) ==
        p.table().integer( 64, true )
    );

    // `T` is not something an author can act on, so the witness is never the parameter itself.
    const Type_id witness = p.bounds().narrowing_witness( p.parameter( 0 ), p.table().integer( 32, true ) );

    REQUIRE( witness.is_valid() );
    REQUIRE( !p.table().is_parameter( witness ) );
}

TEST_CASE( "bounds_measure_the_narrowest_integer_a_parameter_may_be", "[sema][bound]" )
{
    Promises p( "void f<T>( const ref T a ) where T : Integral { }\n"
                "void g<U>( const ref U a ) where U : Floating { }\n" );

    REQUIRE( p.bounds().narrowest_admissible_width( p.parameter( 0 ) ) == 8 );
    REQUIRE( p.bounds().narrowest_admissible_width( p.parameter( 1 ) ) == 0 ); // no integer is admissible at all
}

TEST_CASE( "bounds_reject_a_literal_the_narrowest_admissible_type_will_not_hold", "[sema][bound]" )
{
    Promises p( "i32 small = 3;\ni32 large = 300;\nvoid f<T>( const ref T a ) where T : Integral { }\n" );

    const Bound_set integral = p.bounds().bounds_for( p.parameter( 0 ) );

    REQUIRE( !p.bounds().type_the_literal_overflows( p.literal( Node_kind::Int_literal, 0 ), false, integral ).is_valid() );

    const Type_id rejects = p.bounds().type_the_literal_overflows( p.literal( Node_kind::Int_literal, 1 ), false, integral );

    REQUIRE( rejects.is_valid() );
    REQUIRE( p.table().get( rejects ).width == 8 ); // the answer an author can act on, not `T`
}

TEST_CASE( "bounds_report_the_strongest_unmet_bound_and_not_the_ones_it_implies", "[sema][bound]" )
{
    Promises p( "struct S { i32 x; };\nvoid f<T>( const ref T a ) where T : Integral { }\n" );

    const Node_id parameter = p.parameter_decl( 0 );

    p.bounds().check_bounds( parameter, p.span( parameter ), p.aggregate( 0 ), "f" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 ); // a struct fails four of them, and only one is the mistake
    REQUIRE( p.rendered().find( "not `Integral`" ) != std::string::npos );
    REQUIRE( p.rendered().find( "Numeric" ) == std::string::npos );
}

TEST_CASE( "bounds_refuse_a_second_where_clause_for_one_parameter", "[sema][bound]" )
{
    Promises p( "void f<T>( const ref T a ) where T : Integral where T : Copyable { }\n" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "duplicate where clause" ) != std::string::npos );
}

TEST_CASE( "bounds_refuse_a_name_that_is_not_a_bound", "[sema][bound]" )
{
    Promises p( "void f<T>( const ref T a ) where T : Printable { }\n" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "unknown bound `Printable`" ) != std::string::npos );
    REQUIRE( p.rendered().find( "Copyable, Equatable" ) != std::string::npos ); // listed from the table, not a string
}

} // namespace

TEST_CASE( "type_checker_checks_bounds_at_the_call_site", "[sema][generic][bound]" )
{
    const auto call = []( std::string_view clause, std::string_view argument, std::string_view value )
    {
        return fmt::format(
            "class Buffer {{ i32 x; Buffer( i32 n ) {{ x = n; }} ~Buffer() {{ }} }};\n"
            "enum Colour {{ Red, Green }};\n"
            "void f<T>( const ref T a ) where T : {} {{ }}\n"
            "i32 main() {{ {} v = {}; f<{}>( v ); return 0; }}\n",
            clause,
            argument,
            value,
            argument
        );
    };

    SECTION( "each bound, met" )
    {
        const Typed copyable( call( "Copyable", "i32", "1" ) );
        const Typed equatable( call( "Equatable", "bool", "true" ) );
        const Typed comparable( call( "Comparable", "i32", "1" ) );
        const Typed numeric( call( "Numeric", "f64", "1.0" ) );
        const Typed integral( call( "Integral", "u8", "1" ) );
        const Typed floating( call( "Floating", "f64", "1.0" ) );

        INFO(
            copyable.rendered() << equatable.rendered() << comparable.rendered() << numeric.rendered() << integral.rendered()
                                << floating.rendered()
        );
        REQUIRE( copyable.clean() );
        REQUIRE( equatable.clean() );
        REQUIRE( comparable.clean() );
        REQUIRE( numeric.clean() );
        REQUIRE( integral.clean() );
        REQUIRE( floating.clean() );
    }

    SECTION( "`Copyable` is what a destructor takes away" )
    {
        const Typed p( call( "Copyable", "Buffer", "Buffer( 1 )" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Buffer` is not `Copyable`" ) != std::string::npos );
    }

    SECTION( "an enum and a pointer are `Equatable` but not `Comparable`" )
    {
        // The split is D30's: a variant can be told from another, but the declaration order it
        // happens to have is not an ordering anyone wrote down. Ditto two pointers.
        const Typed equatable( call( "Equatable", "Colour", "Colour::Red" ) );
        const Typed comparable( call( "Comparable", "Colour", "Colour::Red" ) );

        INFO( equatable.rendered() << comparable.rendered() );
        REQUIRE( equatable.clean() );
        REQUIRE( comparable.errors() == 1 );
        REQUIRE( comparable.rendered().find( "`Colour` is not `Comparable`" ) != std::string::npos );
    }

    SECTION( "a bound the argument misses names the bound and what would meet it" )
    {
        const Typed p( call( "Integral", "f64", "1.0" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`f64` is not `Integral`, and `f` requires it of `T`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "it has to be an integer" ) != std::string::npos );
    }

    SECTION( "one mistake is reported once, not once per implied bound" )
    {
        // closure() stored `Integral` as `Integral | Numeric | Comparable | Equatable`, so a struct
        // fails all four. Only the one the author wrote is a thing they can act on.
        const Typed p( call( "Integral", "Buffer", "Buffer( 1 )" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`Integral`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`Numeric`" ) == std::string::npos );
        REQUIRE( p.rendered().find( "`Comparable`" ) == std::string::npos );
        REQUIRE( p.rendered().find( "`Equatable`" ) == std::string::npos );
    }

    SECTION( "two unmet bounds that imply nothing of each other are both reported" )
    {
        // Two things to fix is two diagnostics: one per compile would be two compiles.
        const Typed p( call( "Copyable & Comparable", "Buffer", "Buffer( 1 )" ) );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "not `Copyable`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "not `Comparable`" ) != std::string::npos );
    }

    SECTION( "an implied bound is met by the one that implies it" )
    {
        // `where T : Numeric` is enough for a parameter that asks only for `Equatable`, because
        // closure() put it there when the clause was read.
        const Typed p( "void needs<T>( const ref T a ) where T : Equatable { }\n"
                       "void has<T>( const ref T a ) where T : Numeric { needs<T>( a ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "implication does not run downhill" )
    {
        const Typed p( "void needs<T>( const ref T a ) where T : Integral { }\n"
                       "void has<T>( const ref T a ) where T : Numeric { needs<T>( a ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not promise `Integral`" ) != std::string::npos );
    }

    SECTION( "a generic forwarding its own `T` is told to widen its clause, not its type" )
    {
        // The type argument is a parameter, so the fix is a clause away rather than a type away.
        // "it has to be an integer" would be advice about the wrong thing entirely.
        const Typed p( "void needs<T>( const ref T a ) where T : Comparable { }\n"
                       "void has<T>( const ref T a ) where T : Equatable { needs<T>( a ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`T` does not promise `Comparable`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "add `Comparable` to the `where` clause for `T` on `has`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "ordering needs a number" ) == std::string::npos );
    }

    SECTION( "an unbounded parameter accepts anything" )
    {
        const Typed p( "void f<T>( const ref T a ) { }\n"
                       "i32 main() { Buffer v = Buffer( 1 ); f<Buffer>( v ); return 0; }\n"
                       "class Buffer { i32 x; Buffer( i32 n ) { x = n; } ~Buffer() { } };" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an unknown type argument is reported once and not measured against the bounds" )
    {
        const Typed p( "void f<T>( const ref T a ) where T : Copyable & Integral { }\n"
                       "i32 main() { i32 v = 1; f<Nope>( v ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "unknown type `Nope`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "requires it of" ) == std::string::npos );
    }

    SECTION( "the diagnostic points at the type argument, not at the whole call" )
    {
        const Typed p( "void f<T, U>( const ref T a, const ref U b ) where U : Integral { }\n"
                       "i32 main() { i32 v = 1; f64 w = 1.0; f<i32, f64>( v, w ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`f64` is not `Integral`, and `f` requires it of `U`" ) != std::string::npos );
    }

    SECTION( "a bound is in hand before the declaration's own annotations are typed" )
    {
        // `out T` asks is_owning_type about `T`, and the only thing that can answer is the `where`
        // clause on the same declaration - which is read ten lines earlier, in the same loop body.
        // Nothing else marks that order as load-bearing, so this pair is what holds it: move the
        // recording after the annotations and the first of these starts reporting.
        const Typed copyable( "void f<T>( out T a ) where T : Copyable { }\ni32 main() { return 0; }" );
        const Typed unbounded( "void f<T>( out T a ) { }\ni32 main() { return 0; }" );

        INFO( copyable.rendered() << unbounded.rendered() );
        REQUIRE( copyable.clean() );
        REQUIRE( unbounded.errors() == 1 );
        REQUIRE(
            unbounded.rendered().find( "`out` is not supported for a type that owns a resource yet" ) != std::string::npos
        );
    }

    SECTION( "the value arguments are still checked after a bound fails" )
    {
        // The binding happens anyway: one unmet bound should not hide a wrong argument.
        const Typed p( "void f<T>( const ref T a ) where T : Integral { }\n"
                       "i32 main() { f64 v = 1.0; f<f64>( nope ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "not `Integral`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "nope" ) != std::string::npos );
    }
}

// The help for a forwarded parameter names the declaration that introduced it. Reading the
// enclosing function instead aborted the compiler outright whenever the annotation was reached by
// the signature pass, which has no enclosing function.
TEST_CASE( "bounds_name_the_declaration_that_owns_the_parameter", "[sema][generic][bound]" )
{
    SECTION( "a field of a generic aggregate names the aggregate" )
    {
        const Typed p( "struct Box<U> where U : Numeric { U v; };\n"
                       "struct Pair<T> { Box<T> b; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "add `Numeric` to the `where` clause for `T` on `Pair`" ) != std::string::npos );
    }

    SECTION( "a call inside a generic function names the function" )
    {
        const Typed p( "void has<U>( const ref U a ) where U : Comparable { }\n"
                       "void forwards<T>( const ref T a ) { has( a ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "add `Comparable` to the `where` clause for `T` on `forwards`" ) != std::string::npos );
    }
}

// D11 wants constraints checked at the definition rather than at each expansion, and that falls
// out of typing the signature: the body is walked once with `T` standing for itself, so an
// operation `T` does not have is rejected *there* rather than inside some instantiation.
//
// With no bound, `T` supports nothing: no operator, no copy, and so no return by value either -
// which is the correct reading of an unbounded parameter, and leaves storing, passing and dropping
// as the whole of what one permits. A `where` clause is what widens it, one promise at a time.
TEST_CASE( "type_checker_checks_a_generic_body_against_its_parameters", "[sema][generic]" )
{
    SECTION( "a body that needs nothing of `T` is accepted" )
    {
        // Storing, passing and dropping need no promise at all - monomorphisation knows the size
        // and the destructor. Taking a `T` and doing nothing with it is the whole of what an
        // unbounded parameter permits.
        const Typed p( "void f<T>( T a ) { }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "returning a `T` by value is a copy, and needs the promise" )
    {
        // The bare parameter of an unbounded `T` is D31's read-only borrow, because `T` may turn
        // out to own something. Returning it by value would hand out a second owner.
        const Typed without( "T id<T>( T a ) { return a; }\ni32 main() { return 0; }" );
        const Typed with( "T id<T>( T a ) where T : Copyable { return a; }\ni32 main() { return 0; }" );

        INFO( without.rendered() << with.rendered() );
        REQUIRE( without.errors() == 1 );
        REQUIRE( without.rendered().find( "cannot return a borrowed value" ) != std::string::npos );
        REQUIRE( with.clean() );
    }

    SECTION( "an unknown name is reported at the definition" )
    {
        const Typed p( "T id<T>( T a ) where T : Copyable { return nonsense; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`nonsense` is not declared" ) != std::string::npos );
    }

    SECTION( "and so is a return of the wrong type" )
    {
        const Typed p( "i32 count<T>( T a ) { return true; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "an operation `T` has not got is refused, with no instantiation needed" )
    {
        // The whole point of D11: reported once, here, rather than once per expansion and pointing
        // at code the author did not write.
        const Typed p( "T twice<T>( T a ) { return a + a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "no operator `+` for `T` and `T`" ) != std::string::npos );
    }

    SECTION( "including a method call" )
    {
        const Typed p( "T grow<T>( T a ) { return a.nope(); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`T` has no methods" ) != std::string::npos );
    }

    SECTION( "and it is reported even with no call site at all" )
    {
        const Typed p( "T twice<T>( T a ) { return a + a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }
}

} // namespace keel
#endif
