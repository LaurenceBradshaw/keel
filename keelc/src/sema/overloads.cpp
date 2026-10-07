// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/overloads.h"

#include <fmt/format.h>
#include <algorithm>
#include <unordered_map>
#include "sema/type_checker.h"

// Overloading: which callable a name means at a call site, what its type arguments are, and
// whether two declarations of one name could ever be told apart.

namespace keel::sema
{

namespace
{
} // namespace

// The types the call wrote, mapped onto the callable's own type parameters. Positional, because
// that is the only correspondence there is - `f<T>` and `f<U>` name theirs differently.
Bindings Overloads::type_bindings( Node_id callable, std::span<const Type_id> arguments ) const
{
    const std::vector<Node_id> parameters = ast_.type_parameters( ast_.type_param_list( callable ) );

    Bindings bindings;

    for( std::size_t i = 0; i < parameters.size() && i < arguments.size(); ++i )
    {
        // Annotations::type_of has already reported an unknown type. Binding the error type keeps
        // every later substitution total, and check() absorbs it at each argument.
        bindings.emplace( types_.type_of( parameters[i] ).v, arguments[i] );
    }

    return bindings;
}

// The bindings the arguments alone justify, or none at all. Silent, because selection *probes*: a
// candidate whose parameters cannot all be deduced is simply not a candidate, and what to say when
// nothing matches is select_overload's to decide. That is the same split candidate_accepts and
// check_call_arguments already have, for the same reason.
//
// The expectation is deliberately absent. Reading it here would be choosing a callable by what its
// result is wanted for, which is selection by return type - the thing §12 refuses outright when it
// forbids two declarations that differ only in what they return.
bool Overloads::deduce_for_candidate(
    Node_id callable, u32 implicit_params, std::span<const Argument_shape> shapes, std::vector<Type_id>& resolved
)
{
    const std::span<const Node_id> params = ast_.params( callable ).subspan( implicit_params );

    Bindings bindings;

    for( std::size_t i = 0; i < params.size() && i < shapes.size(); ++i )
    {
        // A shape that is not `Typed` carries no type, and deduce() reads nothing from one - which
        // is where a literal saying nothing actually happens. Two arguments that disagree need no
        // test here either: the first binding wins, and candidate_accepts then finds the second
        // argument does not have the parameter's type.
        Bindings deduced;

        if( !table_.deduce( types_.type_of( params[i] ), shapes[i].type, deduced ) )
        {
            continue;
        }

        for( const auto& [parameter, type] : deduced )
        {
            bindings.emplace( parameter, type );
        }
    }

    for( const Node_id parameter : ast_.type_parameters( ast_.type_param_list( callable ) ) )
    {
        const auto found = bindings.find( types_.type_of( parameter ).v );

        if( found == bindings.end() )
        {
            return false;
        }

        resolved.push_back( found->second );
    }

    return true;
}

// Whether `candidate` takes as many arguments and type arguments as the call wrote.
bool Overloads::fits( Node_id call, Node_id candidate, u32 implicit_params ) const
{
    const std::span<const Node_id> arguments = ast_.arguments( call );
    const std::size_t              written   = ast_.type_args( call ).size();
    const std::size_t              params    = ast_.params( candidate ).size() - implicit_params;

    // A call that wrote type arguments means them, so a candidate has to take exactly that many
    // - which keeps a non-generic out of `f<i32>( x )`. A call that wrote none says nothing
    // about genericity, so both kinds stay and the types decide below.
    return params == arguments.size() &&
           ( written == 0 || ast_.type_parameters( ast_.type_param_list( candidate ) ).size() == written );
}

// §12: inferred where possible, written where not. Two sources, and they are not equal - the
// arguments decide, and the expectation fills only what they left. `i64 x = id( a );` on an `i32`
// therefore instantiates `id<i32>` and widens the result, exactly as the written `id<i32>( a )`
// does: an expectation is where a value is going, not a constraint on how it was made.
//
// False with a diagnostic already reported. `resolved` comes back as the vector a written list
// would have produced, so everything downstream - the bounds, the instantiation, the call graph,
// the mangled name - cannot tell a deduced call from a written one.
bool Overloads::deduce_type_arguments(
    Node_id                         callable,
    u32                             implicit_params,
    std::span<const Argument_shape> shapes,
    std::span<const Node_id>        arguments,
    Type_id                         result,
    Type_id                         expectation,
    std::string_view                name,
    Span                            at,
    std::vector<Type_id>&           resolved,
    std::vector<Node_id>&           bound_by
)
{
    const std::vector<Node_id>     parameters = ast_.type_parameters( ast_.type_param_list( callable ) );
    const std::span<const Node_id> params     = ast_.params( callable ).subspan( implicit_params );

    Bindings                         bindings;
    std::unordered_map<u32, Node_id> from; // which argument bound each parameter, for the messages

    for( std::size_t i = 0; i < params.size() && i < shapes.size() && i < arguments.size(); ++i )
    {
        // Deduced into a scratch map and merged only on success: a partial match leaves bindings
        // behind that no argument actually justified.
        Bindings deduced;

        if( !table_.deduce( types_.type_of( params[i] ), shapes[i].type, deduced ) )
        {
            continue; // the argument does not match the parameter's shape; check() reports that
        }

        // Merged in declaration order rather than the map's, so which of two disagreements is
        // reported does not depend on hashing.
        for( const Node_id parameter : parameters )
        {
            const auto found = deduced.find( types_.type_of( parameter ).v );

            if( found == deduced.end() )
            {
                continue;
            }

            const auto already = bindings.find( found->first );

            if( already == bindings.end() )
            {
                bindings.emplace( found->first, found->second );
                from.emplace( found->first, arguments[i] );
                continue;
            }

            if( already->second == found->second || table_.references_error( already->second ) ||
                table_.references_error( found->second ) )
            {
                continue;
            }

            reporter_.error_at(
                ast_.span( arguments[i] ),
                fmt::format(
                    "`{}` cannot be both `{}` and `{}`",
                    interner_.text( ast_.name( parameter ) ),
                    table_.name( already->second ),
                    table_.name( found->second )
                ),
                fmt::format( "an earlier argument already made it `{}`", table_.name( already->second ) )
            );

            return false;
        }
    }

    // Whatever the arguments left open. `T make<T>()` has no argument to read at all, which is the
    // case this exists for. `result` rather than the callable's own type because a constructor
    // produces the aggregate rather than returning anything - `Box<i32> b = Box( 7 );` reads its
    // instance from the expectation exactly as `Box { 7 }` does.
    if( result.is_valid() && expectation.is_valid() && !table_.is_error( expectation ) )
    {
        Bindings deduced;

        if( table_.deduce( result, expectation, deduced ) )
        {
            for( const auto& [parameter, type] : deduced )
            {
                bindings.emplace( parameter, type );
            }
        }
    }

    // All or nothing: a partial list is a rule nobody can recite, so one parameter left open sends
    // the author to the explicit form for the whole call.
    resolved.reserve( parameters.size() );
    bound_by.reserve( parameters.size() );

    for( const Node_id parameter : parameters )
    {
        const auto found = bindings.find( types_.type_of( parameter ).v );

        if( found == bindings.end() )
        {
            reporter_.error_at(
                at,
                fmt::format( "nothing here says what `{}` is in `{}`", interner_.text( ast_.name( parameter ) ), name ),
                fmt::format( "write the type arguments, as in `{}<i32>( ... )`", name )
            );

            resolved.clear();
            bound_by.clear();
            return false;
        }

        resolved.push_back( found->second );

        const auto bound = from.find( found->first );

        bound_by.push_back( bound == from.end() ? Node_id {} : bound->second );
    }

    return true;
}

// D31's marker is part of what the call says, so it selects as much as the type does. The exemption
// is the one the diagnostic applies too: on a non-owning type `move` is the caller's own assertion
// about its variable, which the callee never sees.
bool Overloads::marker_accepts( Node_id param, Keyword given, Type_id expected )
{
    const Keyword wanted = ast_.call_marker( param );

    if( wanted == given )
    {
        return true;
    }

    const bool about_ownership =
        ( wanted == Keyword::Count || wanted == Keyword::Move ) && ( given == Keyword::Count || given == Keyword::Move );

    return about_ownership && bounds_.satisfies( expected, Bound::Copyable );
}

// Exactly, or with `widen` set, through widens(): §6.7's second tier, asked only when the first
// found nothing.
bool Overloads::candidate_accepts(
    Node_id callable, u32 implicit_params, std::span<const Argument_shape> shapes, const Bindings& bindings, bool widen
)
{
    const std::span<const Node_id> params = ast_.params( callable ).subspan( implicit_params );

    for( std::size_t i = 0; i < params.size() && i < shapes.size(); ++i )
    {
        const Type_id expected = table_.substitute( types_.type_of( params[i] ), bindings );

        // A parameter whose own type failed to resolve accepts anything: the error is already
        // reported, and refusing here would answer it with a second one about the call.
        if( table_.is_error( expected ) )
        {
            continue;
        }

        if( !marker_accepts( params[i], shapes[i].marker, expected ) )
        {
            return false;
        }

        switch( shapes[i].kind )
        {
        case Argument_kind::Typed:
            if( shapes[i].type != expected && !( widen && widens( params[i], shapes[i].type, expected ) ) )
            {
                return false;
            }
            break;

        case Argument_kind::Integer:
            if( !table_.is_integer( expected ) )
            {
                return false;
            }
            break;

        case Argument_kind::Floating:
            if( !table_.is_float( expected ) )
            {
                return false;
            }
            break;

        case Argument_kind::Anything:
            break;
        }
    }

    return true;
}

// §6.4's assignment widening, numeric and by value only: a borrow is the caller's variable (D31).
// An integer reaches a float that holds it, as it does through one candidate; distance() ranks it
// behind every integer.
bool Overloads::widens( Node_id param, Type_id from, Type_id to ) const
{
    const auto numeric = [&]( Type_id type ) { return table_.is_integer( type ) || table_.is_float( type ); };

    return ast_.parameter_mode( param ) == Keyword::Count && numeric( from ) && numeric( to ) && table_.holds( from, to );
}

// How far `from` travels to reach `to`; smaller is closer, and 0 is exact or not a number.
u32 Overloads::distance( Type_id from, Type_id to ) const
{
    if( from == to )
    {
        return 0;
    }

    const bool from_integer = table_.is_integer( from );
    const bool to_integer   = table_.is_integer( to );
    const bool from_float   = table_.is_float( from );
    const bool to_float     = table_.is_float( to );

    if( !( ( from_integer || from_float ) && ( to_integer || to_float ) ) )
    {
        return 0;
    }

    const Type& source = table_.get( from );
    const Type& target = table_.get( to );

    u32 kind  = from_integer != to_integer ? 1 : 0;
    u32 width = target.width;
    u32 sign  = from_integer && to_integer && source.is_signed != target.is_signed ? 1 : 0;

    // Kind outranks any width (129 < 256), and width outranks signedness: §6.7's order.
    return ( kind << 8 ) | ( width << 1 ) | sign;
}

std::vector<Type_id> Overloads::parameter_types( Node_id callable, u32 implicit_params, const Bindings& bindings )
{
    std::vector<Type_id> result;
    for( const Node_id param : ast_.params( callable ).subspan( implicit_params ) )
    {
        result.push_back( table_.substitute( types_.type_of( param ), bindings ) );
    }

    return result;
}

// Whether `first` is no further than `second` for any argument and closer for one. A literal says
// nothing, so it never decides.
bool Overloads::beats( std::span<const Type_id> first, std::span<const Type_id> second, std::span<const Argument_shape> shapes )
    const
{
    bool closer = false;
    u32  i      = 0;
    while( i < shapes.size() && i < first.size() )
    {
        if( shapes[i].kind != Argument_kind::Typed )
        {
            ++i;
            continue;
        }

        u32 near = distance( shapes[i].type, first[i] );
        u32 far  = distance( shapes[i].type, second[i] );

        if( near > far )
        {
            return false;
        }

        if( near < far )
        {
            closer = true;
        }

        ++i;
    }

    return closer;
}

// Every widened candidate nothing beats: one is the choice, several are the ambiguity.
std::vector<Node_id> Overloads::closest(
    std::span<const Node_id> widened, std::span<const std::vector<Type_id>> targets, std::span<const Argument_shape> shapes
) const
{
    std::vector<Node_id> result;

    for( std::size_t k = 0; k < widened.size(); ++k )
    {
        bool beaten = false;

        for( std::size_t j = 0; j < widened.size(); ++j )
        {
            if( j == k )
            {
                continue;
            }

            if( beats( targets[j], targets[k], shapes ) )
            {
                beaten = true;
                break;
            }
        }

        if( !beaten )
        {
            result.push_back( widened[k] );
        }
    }

    return result;
}

// Both tiers over `viable`, reporting nothing: the exact matches, else the closest widenings.
std::vector<Node_id> Overloads::matching(
    Node_id                         call,
    std::span<const Node_id>        viable,
    u32                             implicit_params,
    Type_id                         instance,
    std::span<const Argument_shape> shapes,
    std::span<const Type_id>        resolved
)
{
    const std::size_t                 written = ast_.type_args( call ).size();
    std::vector<Node_id>              widened_candidates;
    std::vector<std::vector<Type_id>> targets;
    std::vector<Node_id>              matching_candidates;

    // A construction's type parameters are the aggregate's, and the instance already says what they
    // are - so `Box<i32> b = Box( 7, true );` settles `T` before an argument is read. That is the
    // receiver's role in select_method rather than selection by return type, which
    // deduce_for_candidate refuses for a function and still refuses here.
    const Bindings from_instance = aggregates_.bindings_of( instance );

    for( const Node_id candidate : viable )
    {
        // Deduced only to ask whether this one could have been meant. What it found is thrown
        // away: the caller deduces again once the callable is settled, with the expectation in
        // hand and with a diagnostic to give, and keeping it here would be two answers to keep
        // in step.
        std::vector<Type_id> deduced;
        const bool           infers = written == 0 && from_instance.empty() && ast_.is_generic( candidate );

        if( infers && !deduce_for_candidate( candidate, implicit_params, shapes, deduced ) )
        {
            continue; // nothing here says what its parameters are, so it is not what was meant
        }

        // What the call wrote or the arguments deduced wins; the instance fills what is left.
        Bindings bindings = type_bindings( candidate, infers ? deduced : resolved );
        bindings.insert( from_instance.begin(), from_instance.end() );

        if( candidate_accepts( candidate, implicit_params, shapes, bindings, false ) )
        {
            matching_candidates.push_back( candidate );
        }
        else if( candidate_accepts( candidate, implicit_params, shapes, bindings, true ) )
        {
            widened_candidates.push_back( candidate );
            targets.push_back( parameter_types( candidate, implicit_params, bindings ) );
        }
    }

    if( matching_candidates.empty() )
    {
        matching_candidates = closest( widened_candidates, targets, shapes );
    }

    return matching_candidates;
}

// The set narrowed by what the call wrote. Empty with a diagnostic already reported, one when that
// settled it, or the rest for the caller to shape its arguments for. Only ever reached with two or
// more candidates: one is the ordinary path, which this must leave exactly as it was.
std::vector<Node_id> Overloads::viable_overloads(
    Node_id call, std::string_view name, std::span<const Node_id> candidates, u32 implicit_params, Type_id instance
)
{
    const std::span<const Node_id> arguments = ast_.arguments( call );
    const std::size_t              written   = ast_.type_args( call ).size();

    // How many arguments and how many type arguments are what the call *says*, so they narrow the
    // set before anything is typed - and typing an argument is what cannot be taken back.
    std::vector<Node_id> viable;

    for( const Node_id candidate : candidates )
    {
        if( fits( call, candidate, implicit_params ) )
        {
            viable.push_back( candidate );
        }
    }

    if( viable.empty() )
    {
        // Which of the two filters emptied it. Both are things the call site wrote, so naming the
        // wrong one would send the author to the wrong half of their own line.
        const bool arity = std::none_of(
            candidates.begin(),
            candidates.end(),
            [&]( Node_id candidate ) { return ast_.params( candidate ).size() - implicit_params == arguments.size(); }
        );

        reporter_.error_at(
            ast_.span( call ),
            arity ? fmt::format( "no `{}` takes {} argument{}", name, arguments.size(), arguments.size() == 1 ? "" : "s" )
                  : fmt::format( "no `{}` takes {} type argument{}", name, written, written == 1 ? "" : "s" ),
            fmt::format(
                "the ones declared take {}", candidate_list( candidates, implicit_params, aggregates_.bindings_of( instance ) )
            )
        );
    }

    return viable;
}

// The rest of the choice, over the shapes the caller reduced the arguments to. `viable` is what the
// call above handed back, so the counts it already ruled on are not asked again.
Node_id Overloads::select_overload(
    Node_id                         call,
    std::string_view                name,
    std::span<const Node_id>        viable,
    u32                             implicit_params,
    Type_id                         instance,
    std::span<const Argument_shape> shapes,
    std::vector<Type_id>&           resolved
)
{
    if( ast_.type_arg_list( call ).is_valid() && resolved.empty() )
    {
        for( const Node_id written_argument : ast_.type_args( call ) )
        {
            resolved.push_back( annotations_.type_of( written_argument ) );
        }
    }

    std::vector<Node_id> matching_candidates = matching( call, viable, implicit_params, instance, shapes, resolved );

    // A non-generic candidate beats a generic one, and two generics are ambiguous: read off the
    // declarations, and settling ties among exact matches or among equally close widenings.
    if( matching_candidates.size() > 1 )
    {
        Node_id     concrete {};
        std::size_t found = 0;

        for( const Node_id candidate : matching_candidates )
        {
            if( !ast_.is_generic( candidate ) )
            {
                concrete = candidate;
                ++found;
            }
        }

        if( found == 1 )
        {
            return concrete; // it has no type arguments, so `resolved` stays as the call left it
        }
    }

    if( matching_candidates.size() == 1 )
    {
        return matching_candidates.front();
    }

    if( matching_candidates.empty() )
    {
        reporter_.error_at(
            ast_.span( call ),
            fmt::format( "no `{}` matches these arguments", name ),
            fmt::format(
                "the ones declared take {}", candidate_list( viable, implicit_params, aggregates_.bindings_of( instance ) )
            )
        );

        return Node_id {};
    }

    // More than one matches. A literal is the usual cause: it carries a family rather than a type,
    // so it cannot choose between two parameters in one family. The other is two widenings each
    // closer for a different argument. Where every one left is a generic the arguments cannot say
    // it either - both fit exactly - and only the type arguments can.
    const bool all_generic = std::all_of(
        matching_candidates.begin(),
        matching_candidates.end(),
        [&]( Node_id candidate ) { return ast_.is_generic( candidate ); }
    );

    reporter_.error_at(
        ast_.span( call ),
        fmt::format( "this call to `{}` is ambiguous", name ),
        fmt::format(
            "more than one matches: {}; {}",
            candidate_list( matching_candidates, implicit_params, aggregates_.bindings_of( instance ) ),
            all_generic ? fmt::format( "write the type arguments, as in `{}<i32>( ... )`", name )
                        : std::string( "write a type the call can be told by" )
        )
    );

    return Node_id {};
}

// The same narrowing, made from a receiver's members rather than from a scope. A method call writes
// no type arguments - the receiver carries them - so arity is the only filter before the types.
std::vector<Node_id> Overloads::viable_methods( Node_id call, Node_id first, Type_id receiver )
{
    std::vector<Node_id> candidates;

    for( Node_id candidate = first; candidate.is_valid(); candidate = next_overload( candidate ) )
    {
        candidates.push_back( candidate );
    }

    if( candidates.size() == 1 )
    {
        return candidates; // one is the ordinary path, and the arity below is check_call_arguments'
    }

    const std::span<const Node_id> arguments = ast_.arguments( call );
    const std::string_view         name      = interner_.text( ast_.name( first ) );
    const Bindings                 bindings  = aggregates_.bindings_of( receiver );

    std::vector<Node_id> viable;

    for( const Node_id candidate : candidates )
    {
        if( ast_.params( candidate ).size() - 1 == arguments.size() )
        {
            viable.push_back( candidate );
        }
    }

    if( viable.empty() )
    {
        reporter_.error_at(
            ast_.span( call ),
            fmt::format( "no `{}` takes {} argument{}", name, arguments.size(), arguments.size() == 1 ? "" : "s" ),
            fmt::format( "the ones declared take {}", candidate_list( candidates, 1, bindings ) )
        );
    }

    return viable;
}

// The rest of that choice, over the shapes the caller reduced the arguments to.
Node_id Overloads::select_method(
    Node_id call, Node_id first, Type_id receiver, std::span<const Node_id> viable, std::span<const Argument_shape> shapes
)
{
    const std::string_view name     = interner_.text( ast_.name( first ) );
    const Bindings         bindings = aggregates_.bindings_of( receiver );

    std::vector<Node_id>              matching;
    std::vector<Node_id>              widened;
    std::vector<std::vector<Type_id>> targets;

    for( const Node_id candidate : viable )
    {
        if( candidate_accepts( candidate, 1, shapes, bindings, false ) )
        {
            matching.push_back( candidate );
        }
        else if( candidate_accepts( candidate, 1, shapes, bindings, true ) )
        {
            widened.push_back( candidate );
            targets.push_back( parameter_types( candidate, 1, bindings ) );
        }
    }

    if( matching.empty() )
    {
        matching = closest( widened, targets, shapes );
    }

    if( matching.size() == 1 )
    {
        return matching.front();
    }

    if( matching.empty() )
    {
        reporter_.error_at(
            ast_.span( call ),
            fmt::format( "no `{}` matches these arguments", name ),
            fmt::format( "the ones declared take {}", candidate_list( viable, 1, bindings ) )
        );

        return Node_id {};
    }

    reporter_.error_at(
        ast_.span( call ),
        fmt::format( "this call to `{}` is ambiguous", name ),
        fmt::format(
            "more than one matches: {}; write a type the call can be told by", candidate_list( matching, 1, bindings )
        )
    );

    return Node_id {};
}

// The arguments against the chosen callable's parameters. `shapes` is empty on the ordinary path,
// and on the overloaded one it says which arguments selection has already typed. What is left for
// the walk comes back in argument order rather than being walked here - §4.3.
std::vector<Argument_work> Overloads::check_call_arguments(
    Node_id                         call,
    Node_id                         callable,
    std::string_view                name,
    u32                             implicit_params,
    const Bindings&                 bindings,
    std::span<const Argument_shape> shapes
)
{
    const std::span<const Node_id> declared  = ast_.params( callable );
    const std::span<const Node_id> params    = declared.subspan( implicit_params );
    const std::span<const Node_id> arguments = ast_.arguments( call );

    std::vector<Argument_work> work;

    if( params.size() != arguments.size() )
    {
        reporter_.error_at(
            ast_.span( ast_.arg_list( call ) ),
            fmt::format(
                "`{}` takes {} argument{}, but {} {} given",
                name,
                params.size(),
                params.size() == 1 ? "" : "s",
                arguments.size(),
                arguments.size() == 1 ? "was" : "were"
            )
        );
    }

    // Check the pairs that do line up even when the count is wrong: one missing argument should
    // not hide a type error in the others. A parameter whose own type failed to resolve holds an
    // invalid Type_id, which check() absorbs.
    const std::size_t shared = std::min( params.size(), arguments.size() );

    for( std::size_t i = 0; i < shared; ++i )
    {
        // The parameter's type with this call's type arguments bound. Every use below is of this
        // rather than of the declared type: the declared one may be `T`, which the caller never
        // wrote and which no diagnostic should name. An empty map makes this the identity, so the
        // ordinary path needs no branch.
        const Type_id expected = table_.substitute( types_.type_of( params[i] ), bindings );

        // Already typed, by selection or by deduction, both of which had to know what the argument
        // was before they could use it. Typing it again would report a mistake inside it twice -
        // but it still has to be *checked*, and only selection did that. Deduction reads an
        // argument without judging it: one that deduces nothing simply deduces nothing.
        if( i >= shapes.size() || !shapes[i].recorded )
        {
            work.push_back( { arguments[i], expected, true } );
        }
        else if( !table_.is_error( shapes[i].type ) && !table_.holds( shapes[i].type, expected ) )
        {
            reporter_.error_at(
                ast_.span( arguments[i] ),
                fmt::format( "expected `{}`, but got `{}`", table_.name( expected ), table_.name( shapes[i].type ) )
            );
        }
    }

    for( std::size_t i = shared; i < arguments.size(); ++i )
    {
        if( i >= shapes.size() || !shapes[i].recorded )
        {
            work.push_back( { arguments[i], {}, false } );
        }
    }

    return work;
}

// D2's markers, after the caller has walked the work above. A second member rather than the second
// half of the first: the borrow rule below reads the argument's recorded type, which is not written
// until the walk this class handed back has run.
void Overloads::check_argument_markers(
    Node_id call, Node_id callable, std::string_view name, u32 implicit_params, const Bindings& bindings
)
{
    const std::span<const Node_id> params    = ast_.params( callable ).subspan( implicit_params );
    const std::span<const Node_id> arguments = ast_.arguments( call );

    for( std::size_t i = 0; i < std::min( params.size(), arguments.size() ); ++i )
    {
        const Type_id expected = table_.substitute( types_.type_of( params[i] ), bindings );

        check_one_argument_marker( arguments[i], parameter_mode_of( ast_, params[i] ), expected, name );
    }
}

// D2: transfer is visible at the call *and* in the signature, and neither alone is enough - a reader
// of one should never have to find the other.
void Overloads::check_one_argument_marker( Node_id argument, Param_mode mode, Type_id expected, std::string_view name )
{
    const Keyword wanted = call_marker_of( mode );
    const Keyword given  = ast_.kind( argument ) == Node_kind::Marker_expr ? ast_.keyword( argument ) : Keyword::Count;

    // D31: a borrow - `ref`, `out` or `const ref` - binds the caller's object itself, so there is no
    // conversion step for a widened copy to live in: `ref u8` and `ref i32` are different bindings.
    // Only a type that would convert reaches it; one that would not is check()'s error already.
    // Above the agreement check, because agreeing is this rule's precondition rather than its
    // exit - and it wants both sides, so one missing marker does not report twice.
    if( ( mode == Param_mode::Ref || mode == Param_mode::Const_ref || mode == Param_mode::Out ) && given == wanted &&
        !table_.references_error( types_.type_of( argument ) ) && !table_.references_error( expected ) &&
        types_.type_of( argument ) != expected && table_.holds( types_.type_of( argument ), expected ) )
    {
        reporter_.error_at(
            ast_.span( argument ),
            fmt::format(
                "cannot borrow `{}` as `{}{}`",
                table_.name( types_.type_of( argument ) ),
                param_spelling( mode ),
                table_.name( expected )
            ),
            "a borrow is the variable itself, so its type must match exactly"
        );
    }

    if( wanted == given )
    {
        return;
    }

    // The exemption is narrow and belongs only to `move`: on a non-owning type it is the
    // caller's own assertion that the source is dead afterwards, which the callee never sees.
    // `ref` changes what the callee is holding, at every type.
    const bool about_ownership =
        ( wanted == Keyword::Count || wanted == Keyword::Move ) && ( given == Keyword::Count || given == Keyword::Move );

    // The substituted type, not the declared one: whether `T` owns is a property of the
    // instantiation, and a bare `Parameter` owns nothing - which would wave every `move`
    // through unmarked.
    if( about_ownership && bounds_.satisfies( expected, Bound::Copyable ) )
    {
        return;
    }

    if( wanted == Keyword::Ref )
    {
        reporter_.error_at( ast_.span( argument ), fmt::format( "`{}` may modify this argument", name ), "write `ref`" );
    }
    else if( given == Keyword::Ref )
    {
        reporter_.error_at( ast_.span( argument ), fmt::format( "`{}` does not modify this argument", name ), "remove `ref`" );
    }
    else if( wanted == Keyword::Move )
    {
        reporter_.error_at(
            ast_.span( argument ), fmt::format( "`{}` takes ownership of this argument", name ), "write `move`"
        );
    }
    else if( given == Keyword::Move )
    {
        reporter_.error_at( ast_.span( argument ), fmt::format( "`{}` borrows this argument", name ), "remove `move`" );
    }
    else if( wanted == Keyword::Out )
    {
        reporter_.error_at( ast_.span( argument ), fmt::format( "`{}` assigns this argument", name ), "write `out`" );
    }
    else if( given == Keyword::Out )
    {
        reporter_.error_at( ast_.span( argument ), fmt::format( "`{}` does not assign this argument", name ), "remove `out`" );
    }
}

bool Overloads::parameters_collide( Node_id first, Node_id second, bool member )
{
    // A member's parameter 0 is the receiver, which the call site never writes - so two that differ
    // only in it, `area()` and `area() const`, are one signature and have to be refused here. Asked
    // of each signature rather than of the kind, and separately: M7's static method is a member with
    // no receiver, so a pair may legitimately disagree about whether there is one to skip.
    const std::span<const Node_id> mine   = ast_.explicit_params( first );
    const std::span<const Node_id> theirs = ast_.explicit_params( second );

    if( mine.size() != theirs.size() )
    {
        return false;
    }

    const std::vector<Node_id> my_parameters    = ast_.type_parameters( ast_.type_param_list( first ) );
    const std::vector<Node_id> their_parameters = ast_.type_parameters( ast_.type_param_list( second ) );

    // A call writes its type arguments, so a different count is something the call site says.
    if( my_parameters.size() != their_parameters.size() )
    {
        return false;
    }

    // `f<T>( T a )` and `f<U>( U a )` are one signature written twice, and their parameters intern
    // to different types - so the second is read through the first's names before comparing.
    Bindings bindings;

    for( std::size_t i = 0; i < my_parameters.size(); ++i )
    {
        bindings.emplace( types_.type_of( their_parameters[i] ).v, types_.type_of( my_parameters[i] ) );
    }

    for( std::size_t i = 0; i < mine.size(); ++i )
    {
        if( ast_.call_marker( mine[i] ) != ast_.call_marker( theirs[i] ) )
        {
            return false;
        }

        const Type_id left  = types_.type_of( mine[i] );
        const Type_id right = table_.substitute( types_.type_of( theirs[i] ), bindings );

        if( left == right )
        {
            continue;
        }

        // Deliberately coarse: `Box<T>` can never be `i32`, and this says it could. Refusing a
        // legal pair is recoverable, accepting an ambiguous one is not.
        // A member call writes no type arguments, so a `T` parameter may become anything and a pair
        // that some instantiation would make identical has to be refused where it is written.
        if( !member || !( table_.mentions_parameter( left ) || table_.mentions_parameter( right ) ) )
        {
            return false;
        }
    }

    return true;
}

void Overloads::check_overloaded_pair( Node_id first, Node_id second, bool member )
{
    if( refused( first ) || refused( second ) )
    {
        return;
    }

    const std::string_view name = interner_.text( ast_.name( second ) );

    // An extern names a symbol someone else defined, and `main` is the program's entry point:
    // both keep their spelling in C, so a second of either has nowhere to differ.
    if( ast_.is_extern( first ) || ast_.is_extern( second ) )
    {
        refuse(
            second,
            fmt::format( "`{}` is defined in C, so it cannot be overloaded", name ),
            reporter_.previous_declaration_note( ast_.span( first ) )
        );

        return;
    }

    if( ast_.kind( second ) == Node_kind::Function_decl && name == "main" )
    {
        refuse( second, "a program has one `main`", reporter_.previous_declaration_note( ast_.span( first ) ) );

        return;
    }

    const bool exact = parameters_collide( first, second, false );

    if( !exact )
    {
        if( !member || !parameters_collide( first, second, true ) )
        {
            return;
        }

        refuse(
            second,
            fmt::format( "`{}` is already declared, and a type argument could make the two identical", name ),
            reporter_.previous_declaration_note( ast_.span( first ) )
        );

        return;
    }

    // Two declarations that differ only in what they return. Worth its own message: the author
    // wrote a difference, and it is not one a call site can act on.
    if( types_.type_of( first ) != types_.type_of( second ) )
    {
        refuse(
            second,
            fmt::format( "`{}` is already declared with these parameters", name ),
            "two of one name must differ in their parameters, not only in what they return"
        );

        return;
    }

    // M7: one takes an object and the other does not. The call spellings differ, so nothing at a
    // call site is ambiguous - but the category tag names the enclosing type either way and the
    // receiver is among the parameters in neither, so the two emit one symbol. Reported before the
    // `const` clause below, which would otherwise blame a keyword whose removal changes nothing.
    if( member && ast_.has_receiver( first ) != ast_.has_receiver( second ) )
    {
        refuse(
            second,
            fmt::format( "`{}` is already declared with these parameters", name ),
            "a `static` method and a method of one name must differ in their parameters"
        );

        return;
    }

    // And the same for a trailing `const`, which is a habit worth naming: it binds the receiver,
    // and a call writes the object rather than how the method holds it.
    if( member && ast_.is_const_method( first ) != ast_.is_const_method( second ) )
    {
        refuse(
            second,
            fmt::format( "`{}` is already declared with these parameters", name ),
            "`const` binds the receiver, which a call site does not write, so it cannot tell two apart"
        );

        return;
    }

    refuse(
        second,
        fmt::format( "`{}` is already declared with these parameters", name ),
        reporter_.previous_declaration_note( ast_.span( first ) )
    );
}

// Every ordered pair, once: the walk starts from each member of a chain and visits only what
// follows it.
void Overloads::check_overload_sets()
{
    for( const Node_id decl : ast_.declarations( ast_.root() ) )
    {
        if( ast_.kind( decl ) == Node_kind::Function_decl )
        {
            for( Node_id other = resolution_.next_overload( decl ); other.is_valid();
                 other         = resolution_.next_overload( other ) )
            {
                check_overloaded_pair( decl, other, false );
            }

            continue;
        }

        if( is_aggregate( ast_.kind( decl ) ) )
        {
            check_aggregate_overloads( decl );
        }
    }
}

void Overloads::check_aggregate_overloads( Node_id decl )
{
    std::vector<Node_id> constructors;

    for( const Node_id member : ast_.members( decl ) )
    {
        if( ast_.kind( member ) == Node_kind::Constructor_decl )
        {
            constructors.push_back( member );
            continue;
        }

        // An operator is declared once, which Signatures::check_operators enforces.
        if( ast_.kind( member ) != Node_kind::Method_decl || interner_.is_operator_name( ast_.name( member ) ) )
        {
            continue;
        }

        for( Node_id other = resolution_.next_overload( member ); other.is_valid(); other = resolution_.next_overload( other ) )
        {
            check_overloaded_pair( member, other, true );
        }
    }

    // Constructors share the type's name rather than a scope, so they have no chain to walk.
    for( std::size_t i = 0; i < constructors.size(); ++i )
    {
        for( std::size_t j = i + 1; j < constructors.size(); ++j )
        {
            check_overloaded_pair( constructors[i], constructors[j], true );
        }
    }
}

// The signature as a call site would have to write it, for a diagnostic that has to say what the
// author could have meant.
std::string Overloads::signature_of( Node_id callable, u32 implicit_params, const Bindings& bindings )
{
    const std::span<const Node_id> params = ast_.params( callable ).subspan( implicit_params );

    // A list of candidates can hold a generic nothing has instantiated - the call wrote the wrong
    // number of type arguments, or none - and substituting a `T` the map has no entry for asserts.
    // Unbound, the declaration is what the author wrote and is what the message should show.
    bool bound = true;

    for( const Node_id parameter : ast_.type_parameters( ast_.type_param_list( callable ) ) )
    {
        bound = bound && bindings.contains( types_.type_of( parameter ).v );
    }

    std::string text = "(";

    for( std::size_t i = 0; i < params.size(); ++i )
    {
        const Keyword marker = ast_.call_marker( params[i] );

        text += fmt::format(
            "{}{}{}",
            i == 0 ? " " : ", ",
            marker == Keyword::Count ? "" : fmt::format( "{} ", interner_.text( Interner::keyword( marker ) ) ),
            table_.name( bound ? table_.substitute( types_.type_of( params[i] ), bindings ) : types_.type_of( params[i] ) )
        );
    }

    return text + ( params.empty() ? ")" : " )" );
}

std::string Overloads::candidate_list( std::span<const Node_id> candidates, u32 implicit_params, const Bindings& bindings )
{
    std::string text;

    for( std::size_t i = 0; i < candidates.size(); ++i )
    {
        if( i != 0 )
        {
            text += i + 1 == candidates.size() ? " and " : ", ";
        }

        text += fmt::format( "`{}`", signature_of( candidates[i], implicit_params, bindings ) );
    }

    return text;
}

void Overloads::record_instantiation( Node_id call, std::size_t instance )
{
    instantiation_of_.emplace( call.v, narrow_cast<u32>( instance ) );
}

std::unordered_map<u32, u32> Overloads::take_instantiations()
{
    return std::move( instantiation_of_ );
}

void Overloads::refuse( Node_id second, std::string message, std::string help )
{
    reporter_.error_at( ast_.name_span( second ), std::move( message ), std::move( help ) );
    refused_.insert( second.v );
}

bool Overloads::refused( Node_id id ) const
{
    return refused_.contains( id.v );
}

Node_id Overloads::next_overload( Node_id id ) const
{
    Node_id next = resolution_.next_overload( id );
    while( refused( next ) )
    {
        next = resolution_.next_overload( next );
    }

    return next;
}

bool Overloads::applies(
    Node_id                         call,
    std::span<const Node_id>        candidates,
    std::span<const Argument_shape> shapes,
    bool                            ignore_markers,
    std::vector<Type_id>&           resolved
)
{
    if( resolved.empty() )
    {
        for( const Node_id written_argument : ast_.type_args( call ) )
        {
            resolved.push_back( annotations_.type_of( written_argument ) );
        }
    }

    // With `ignore_markers`, a missing `ref` is a mistake to report against this set rather than a
    // reason to call a by-value candidate in another.
    for( const Node_id candidate : candidates )
    {
        if( !fits( call, candidate, 0 ) )
        {
            continue;
        }

        std::vector<Argument_shape>    unmarked( shapes.begin(), shapes.end() );
        const std::span<const Node_id> params = ast_.params( candidate );

        for( std::size_t i = 0; ignore_markers && i < unmarked.size() && i < params.size(); ++i )
        {
            unmarked[i].marker = ast_.call_marker( params[i] );
        }

        if( !matching( call, std::span( &candidate, 1 ), 0, Type_id {}, unmarked, resolved ).empty() )
        {
            return true;
        }
    }

    return false;
}

void Overloads::refuse_both(
    Node_id call, std::string_view name, std::span<const Node_id> own, std::span<const Node_id> prelude
)
{
    std::string message = fmt::format( "no `{}` matches these arguments", name );
    std::string help    = fmt::format(
        "the ones declared take {}; the prelude's take {}", candidate_list( own, 0, {} ), candidate_list( prelude, 0, {} )
    );

    reporter_.error_at( ast_.span( call ), std::move( message ), std::move( help ) );
}

} // namespace keel::sema

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

// `Buffer( 16 )` is a Call_expr whose callee names a type rather than a function. Its result is the
// type itself, and its parameters are the constructor's with the receiver skipped.
// §12. Two callables may share a name when a call site can tell them apart. Every case below is
// one reading of that: what the call says, what it cannot say, and what is refused for saying
// nothing at all.
TEST_CASE( "type_checker_chooses_between_overloads", "[sema][overload]" )
{
    // Every case below reads the answer off the *return* type, and the two return types are `bool`
    // and `i32`, which §6.4 widens into each other in neither direction - so each assignment type
    // checks only if the call chose the overload it was meant to.
    SECTION( "by the argument's own type" )
    {
        const Typed p( "i32 f( i32 a ) { return 1; }\nbool f( f64 a ) { return true; }\n"
                       "i32 main() { f64 d = 1.0; bool b = f( d ); return f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and the wrong one really would be refused" )
    {
        const Typed p( "i32 f( i32 a ) { return 1; }\nbool f( f64 a ) { return true; }\n"
                       "i32 main() { f64 d = 1.0; i32 n = f( d ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "expected `i32`, but got `bool`" ) != std::string::npos );
    }

    SECTION( "by how many arguments there are" )
    {
        const Typed p( "i32 f( i32 a ) { return 1; }\nbool f( i32 a, i32 b ) { return true; }\n"
                       "i32 main() { bool b = f( 1, 2 ); return f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // D26: a literal has a value rather than a type, and here the context that would give it one is
    // what is being chosen - so it narrows the set to a family and no further.
    SECTION( "a literal carries the family it is written in" )
    {
        const Typed p( "i32 f( i32 a ) { return 1; }\nbool f( f64 a ) { return true; }\n"
                       "i32 main() { bool b = f( 1.5 ); return f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and cannot choose between two in one family" )
    {
        const Typed p( "i32 f( i32 a ) { return 1; }\nf64 f( i64 a ) { return 2.0; }\n"
                       "i32 main() { f( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "is ambiguous" ) != std::string::npos );
    }

    // D31's marker is part of what the call writes, so it selects as much as the type does. These
    // two are also the pair that mangled alike until overloading made it reachable.
    SECTION( "by the marker the call writes" )
    {
        const Typed p( "i32 f( i32 a ) { return 1; }\nbool f( ref i32 a ) { return true; }\n"
                       "i32 main() { i32 x = 0; bool b = f( ref x ); return f( x ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a borrow and a pointer are two parameters" )
    {
        const Typed p( "i32 f( ref i32 a ) { return 1; }\nbool f( i32* a ) { return true; }\n"
                       "i32 main() { i32 x = 0; i32* q = &x; bool b = f( q ); return f( ref x ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and one candidate still widens, exactly as before" )
    {
        const Typed p( "void f( i64 a ) { }\ni32 main() { u8 b = 1; f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a wrong count names the count" )
    {
        const Typed p( "void f( i32 a ) { }\nvoid f( f64 a ) { }\ni32 main() { f( 1, 2 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no `f` takes 2 arguments" ) != std::string::npos );
    }

    // `move` on a non-owning type is the caller's own assertion about its variable, which the
    // callee never sees - so selection applies the same exemption the diagnostic does, and a
    // marker that means nothing to the callee cannot stop a candidate matching.
    SECTION( "a `move` on a type that owns nothing still selects" )
    {
        const Typed p( "i32 f( i32 a ) { return 1; }\nbool f( f64 a ) { return true; }\n"
                       "i32 main() { i32 x = 1; return f( move x ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // Choosing has to type the arguments, and the chosen candidate's parameters are then checked
    // against them - so an argument typed twice would report a mistake inside it twice.
    SECTION( "an argument is typed once, however many candidates there were" )
    {
        const Typed p( "void f( i32 a ) { }\nvoid f( f64 a ) { }\ni32 g( i32 n ) { return n; }\n"
                       "i32 main() { f( g( true ) ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    // A local shadows the whole set, because lookup finds the local and never reaches it.
    SECTION( "a local of the same name shadows every candidate" )
    {
        const Typed p( "void f( i32 a ) { }\nvoid f( f64 a ) { }\ni32 main() { i32 f = 1; f( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "is not callable" ) != std::string::npos );
    }

    SECTION( "a generic and a plain one of a name, told apart by what the call writes" )
    {
        const Typed p( "i32 f( i32 a ) { return 1; }\nbool f<T>( T a ) where T : Copyable { return true; }\n"
                       "i32 main() { bool b = f<bool>( true ); return f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// §6.7's second tier. Each case reads the choice off the return type, `i32` for the one meant and
// `bool` for the other, which §6.4 widens into each other in neither direction.
TEST_CASE( "overloads_widen_when_nothing_matches_exactly", "[sema][overload]" )
{
    SECTION( "to the one that holds the argument" )
    {
        const Typed p( "i32 f( i64 a ) { return 1; }\nbool f( bool a ) { return true; }\n"
                       "i32 main() { u8 b = 1; i32 n = f( b ); return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an exact match still wins over a closer-looking widening" )
    {
        const Typed p( "i32 f( i64 a ) { return 1; }\nbool f( i32 a ) { return true; }\n"
                       "i32 main() { i64 b = 1; i32 n = f( b ); return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an integer stays an integer before it becomes a float" )
    {
        const Typed p( "i32 f( i64 a ) { return 1; }\nbool f( f64 a ) { return true; }\n"
                       "i32 main() { i32 b = 1; i32 n = f( b ); return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "however wide the integer and however narrow the float" )
    {
        const Typed p( "i32 f( u64 a ) { return 1; }\nbool f( f32 a ) { return true; }\n"
                       "i32 main() { u8 b = 1; f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "t:1" );
    }

    SECTION( "but an integer becomes a float where nothing else holds it" )
    {
        const Typed p( "f64 root( f64 x ) { return x; }\nf32 root( f32 x ) { return x; }\n"
                       "i32 main() { i32 n = 4; root( n ); u16 s = 4; root( s ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "t:1" ); // an `f32` holds 24 bits, so only `f64` holds an `i32`
        REQUIRE( p.chose( 1 ) == "t:2" ); // and both hold a `u16`, so the narrower wins
    }

    SECTION( "as it does through a single candidate" )
    {
        const Typed p( "f64 root( f64 x ) { return x; }\n"
                       "i32 main() { i32 n = 4; root( n ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and never to a float that would round it" )
    {
        const Typed p( "f64 root( f64 x ) { return x; }\nf32 root( f32 x ) { return x; }\n"
                       "i32 main() { i64 n = 4; root( n ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no `root` matches these arguments" ) != std::string::npos );
    }

    SECTION( "nor a float to an integer" )
    {
        const Typed p( "i32 f( i64 a ) { return 1; }\nbool f( bool a ) { return true; }\n"
                       "i32 main() { f32 b = 1.0; f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no `f` matches these arguments" ) != std::string::npos );
    }

    SECTION( "nor a `bool` to a number" )
    {
        const Typed p( "i32 f( i64 a ) { return 1; }\nbool f( f64 a ) { return true; }\n"
                       "i32 main() { bool b = true; f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "then the narrowest, whatever its signedness" )
    {
        const Typed p( "i32 f( i16 a ) { return 1; }\nbool f( u32 a ) { return true; }\n"
                       "i32 main() { u8 b = 1; i32 n = f( b ); return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "then the one that keeps the signedness" )
    {
        const Typed p( "i32 f( u16 a ) { return 1; }\nbool f( i16 a ) { return true; }\n"
                       "i32 main() { u8 b = 1; i32 n = f( b ); return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "so a `u32` reaches the unsigned of two 64-bit integers" )
    {
        const Typed p( "i32 f( u64 a ) { return 1; }\nbool f( i64 a ) { return true; }\n"
                       "i32 main() { u32 b = 1; i32 n = f( b ); return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a float to the narrowest float" )
    {
        const Typed p( "i32 f( f64 a ) { return 1; }\nbool f( bool a ) { return true; }\n"
                       "i32 main() { f32 b = 1.0; i32 n = f( b ); return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "never narrowing, and never signed to unsigned" )
    {
        const Typed p( "i32 f( i16 a ) { return 1; }\nbool f( u64 a ) { return true; }\n"
                       "i32 main() { i32 b = 1; f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no `f` matches these arguments" ) != std::string::npos );
    }

    SECTION( "a literal is ambiguous before widening is asked" )
    {
        const Typed p( "void f( i64 a ) { }\nvoid f( u64 a ) { }\ni32 main() { f( 5 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "is ambiguous" ) != std::string::npos );
    }

    SECTION( "a generic that fits exactly beats a widening" )
    {
        const Typed p( "i32 f( i64 a ) { return 1; }\nbool f<T>( T a ) where T : Copyable { return true; }\n"
                       "i32 main() { i32 b = 1; bool r = f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "with two arguments, one closer and none further wins" )
    {
        const Typed p( "bool f( i64 a, i64 b ) { return true; }\ni32 f( i32 a, i64 b ) { return 1; }\n"
                       "i32 main() { i16 a = 1; i32 b = 1; i32 n = f( a, b ); return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and one closer on each side is ambiguous" )
    {
        const Typed p( "void f( i32 a, i64 b ) { }\nvoid f( i64 a, i32 b ) { }\n"
                       "i32 main() { i16 a = 1; f( a, a ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "is ambiguous" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`( i32, i64 )` and `( i64, i32 )`" ) != std::string::npos );
    }

    SECTION( "a `const ref` does not widen" )
    {
        const Typed p( "i32 f( const ref i64 a ) { return 1; }\nbool f( bool a ) { return true; }\n"
                       "i32 main() { i32 b = 1; f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no `f` matches these arguments" ) != std::string::npos );
    }

    SECTION( "a method widens too" )
    {
        const Typed p( "class C { i32 x; C() { x = 0; }\n"
                       "public i32 at( i64 n ) { return 1; } public bool at( bool n ) { return true; } };\n"
                       "i32 main() { C c = C(); i8 b = 1; i32 n = c.at( b ); return n; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and so does a constructor" )
    {
        const Typed p( "class C { public i32 x; C( i64 v ) { x = 1; } C( bool v ) { x = 2; } };\n"
                       "i32 main() { u16 b = 1; C c = C( b ); return c.x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// D45: the program's set first, by both tiers, and the prelude's only when it has nothing viable.
// `chose` names the declaration each call went to, so no section infers it from a return type.
TEST_CASE( "overloads_fall_back_to_the_prelude's_set", "[sema][overload][prelude]" )
{
    constexpr std::string_view prelude = "bool f( i64 n ) { return true; }\nbool f( bool b ) { return b; }\n";

    SECTION( "the prelude's set alone is unchanged" )
    {
        const Typed p( "i32 main() { f( 1 ); f( true ); return 0; }", prelude );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "<prelude>:1" );
        REQUIRE( p.chose( 1 ) == "<prelude>:2" );
    }

    SECTION( "the prelude's set is tried when the program's matches no type" )
    {
        const Typed p(
            "struct Point { i32 x; };\ni32 f( Point p ) { return p.x; }\n"
            "i32 main() { Point q = Point { 1 }; f( 5 ); f( true ); f( q ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "<prelude>:1" );
        REQUIRE( p.chose( 1 ) == "<prelude>:2" );
        REQUIRE( p.chose( 2 ) == "t:2" );
    }

    SECTION( "and when it matches no count" )
    {
        const Typed p(
            "i32 f( i64 a, i64 b ) { return 1; }\n"
            "i32 main() { f( 1 ); f( 1, 2 ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "<prelude>:1" );
        REQUIRE( p.chose( 1 ) == "t:1" );
    }

    SECTION( "and when one argument fits it and another does not" )
    {
        const Typed p(
            "i32 f( i64 n ) { return 1; }\n"
            "i32 main() { f( true ); f( 1 ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "<prelude>:2" );
        REQUIRE( p.chose( 1 ) == "t:1" );
    }

    SECTION( "the prelude's set widens once it is the one tried" )
    {
        const Typed p(
            "struct Point { i32 x; };\ni32 f( Point p ) { return p.x; }\n"
            "i32 main() { u8 x = 1; f( x ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "<prelude>:1" );
    }

    SECTION( "to its closest candidate" )
    {
        const Typed p(
            "struct Point { i32 x; };\ni32 f( Point p ) { return p.x; }\n"
            "i32 main() { i8 x = 1; f( x ); return 0; }",
            "bool f( i64 n ) { return true; }\nbool f( i32 n ) { return false; }\n"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "<prelude>:2" );
    }

    SECTION( "and the result is the prelude's candidate's" )
    {
        const Typed p(
            "struct Point { i32 x; };\ni32 f( Point p ) { return p.x; }\n"
            "i32 main() { i32 n = f( 5 ); return n; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "expected `i32`, but got `bool`" ) != std::string::npos );
    }

    SECTION( "a generic in the prelude's set is reached" )
    {
        const Typed p(
            "struct Point { i32 x; };\ni32 f( Point p ) { return p.x; }\n"
            "i32 main() { u16 x = 1; f( x ); return 0; }",
            "bool f<T>( T x ) { return true; }\n"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "<prelude>:1" );
    }

    SECTION( "and so is one the call names the type arguments of" )
    {
        const Typed p(
            "struct Point { i32 x; };\ni32 f( Point p ) { return p.x; }\n"
            "i32 main() { f<i64>( 5 ); return 0; }",
            "bool f<T>( T x ) { return true; }\n"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "<prelude>:1" );
    }

    SECTION( "an argument is typed once, whichever set is tried" )
    {
        const Typed p(
            "struct Point { i32 x; };\ni32 f( Point p ) { return p.x; }\n"
            "i32 main() { f( 1 + true ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no operator `+`" ) != std::string::npos );
    }
}

// The other half of D45: a set with anything viable keeps the call, however much closer the
// prelude's would have been. A flat merge would get every section here wrong.
TEST_CASE( "overloads_keep_the_program's_set_when_it_takes_the_call", "[sema][overload][prelude]" )
{
    constexpr std::string_view prelude = "bool f( i64 n ) { return true; }\nbool f( bool b ) { return b; }\n";

    SECTION( "when it matches exactly what the prelude's also does" )
    {
        const Typed p(
            "i32 f( i64 n ) { return 1; }\n"
            "i32 main() { i64 x = 1; f( x ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "t:1" );
    }

    SECTION( "when only a widening matches, and the prelude's would match exactly" )
    {
        const Typed p(
            "i32 f( i64 n ) { return 1; }\n"
            "i32 main() { i32 x = 1; f( x ); return 0; }",
            "bool f( i32 n ) { return true; }\n"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "t:1" );
    }

    SECTION( "when a literal fits both families" )
    {
        const Typed p(
            "i32 f( i32 n ) { return 1; }\n"
            "i32 main() { f( 5 ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "t:1" );
    }

    SECTION( "when its candidate is a generic and the prelude's is not" )
    {
        const Typed p(
            "i32 f<T>( T x ) { return 1; }\n"
            "i32 main() { f( true ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "t:1" );
    }

    SECTION( "when the call names type arguments only its own candidate takes" )
    {
        const Typed p(
            "i32 f<T>( T x ) { return 1; }\n"
            "i32 main() { f<i64>( 5 ); return 0; }",
            "bool f<T>( T x ) { return true; }\n"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "t:1" );
    }

    SECTION( "and its own ambiguity is reported, not fallen through" )
    {
        const Typed p(
            "i32 f( i32 n ) { return 1; }\ni32 f( i64 n ) { return 2; }\n"
            "i32 main() { f( 5 ); return 0; }",
            "bool f( f64 d ) { return true; }\n"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "this call to `f` is ambiguous" ) != std::string::npos );
        REQUIRE( p.rendered().find( "f64" ) == std::string::npos );
        REQUIRE( p.chose( 0 ).empty() );
    }

    // A marker the call left off is a mistake in the call, not a sign it meant the prelude.
    SECTION( "and so is a missing `ref`" )
    {
        const Typed p(
            "i32 f( ref i64 n ) { return 1; }\n"
            "i32 main() { i64 x = 1; f( x ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "ref" ) != std::string::npos );
        REQUIRE( p.chose( 0 ) == "t:1" );
    }

    SECTION( "and a missing `out`" )
    {
        const Typed p(
            "void f( out i64 n ) { n = 1; }\n"
            "i32 main() { i64 x; f( x ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "out" ) != std::string::npos );
        REQUIRE( p.chose( 0 ) == "t:1" );
    }

    SECTION( "and a `ref` written where its candidate takes a copy" )
    {
        const Typed p(
            "i32 f( i64 n ) { return 1; }\n"
            "i32 main() { i64 x = 1; f( ref x ); return 0; }",
            "bool f( ref i64 n ) { return true; }\n"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.chose( 0 ) == "t:1" );
    }
}

TEST_CASE( "overloads_report_a_call_neither_set_takes", "[sema][overload][prelude]" )
{
    constexpr std::string_view prelude = "bool f( i64 n ) { return true; }\nbool f( bool b ) { return b; }\n";
    constexpr std::string_view both    = "the ones declared take `( Point )`; the prelude's take `( i64 )` and `( bool )`";

    SECTION( "by type, listing both sets" )
    {
        const Typed p(
            "struct Point { i32 x; };\ni32 f( Point p ) { return p.x; }\n"
            "i32 main() { f( 1.5 ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no `f` matches these arguments" ) != std::string::npos );
        REQUIRE( p.rendered().find( both ) != std::string::npos );
        REQUIRE( p.chose( 0 ).empty() );
    }

    SECTION( "by count" )
    {
        const Typed p(
            "struct Point { i32 x; };\ni32 f( Point p ) { return p.x; }\n"
            "i32 main() { f( 1, 2 ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no `f` matches these arguments" ) != std::string::npos );
        REQUIRE( p.rendered().find( both ) != std::string::npos );
    }

    // Never answered with a by-value candidate in the prelude's set: the call wrote `ref`.
    SECTION( "a borrow its own set refuses for its width" )
    {
        const Typed p(
            "i32 f( ref i64 n ) { return 1; }\n"
            "i32 main() { i32 x = 1; f( ref x ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no `f` matches these arguments" ) != std::string::npos );
        REQUIRE( p.chose( 0 ).empty() );
    }

    SECTION( "an ambiguity in the prelude's set names only its candidates" )
    {
        const Typed p(
            "struct Point { i32 x; };\ni32 f( Point p ) { return p.x; }\n"
            "i32 main() { f( 5 ); return 0; }",
            "bool f( i32 n ) { return true; }\nbool f( i64 n ) { return false; }\n"
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "this call to `f` is ambiguous" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`( i32 )` and `( i64 )`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "Point" ) == std::string::npos );
    }
}

// What is not a function's overload set does not fall back: it shadows, as any other name does.
TEST_CASE( "overloads_fall_back_only_from_a_function", "[sema][overload][prelude]" )
{
    constexpr std::string_view prelude = "bool f( i64 n ) { return true; }\n";

    SECTION( "a struct of the name shadows it" )
    {
        const Typed p(
            "struct f { i32 x; };\n"
            "i32 main() { f( 5 ); return 0; }",
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`f` has no constructor" ) != std::string::npos );
    }

    SECTION( "and so does a local" )
    {
        const Typed p( "i32 main() { i32 f = 1; f( 5 ); return 0; }", prelude );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`f` is not callable" ) != std::string::npos );
    }

    SECTION( "and so does a parameter" )
    {
        const Typed p( "i32 g( fn( bool ) -> i32 f ) { return f( 5 ); }\ni32 main() { return 0; }", prelude );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }
}

// A package's own unqualified call falls back like the program's; a qualified one names a single
// package and never does, even where that package's own calls would.
TEST_CASE( "overloads_never_fall_back_through_a_qualifier", "[sema][overload][prelude][packages]" )
{
    constexpr std::string_view prelude = "bool f( i64 n ) { return true; }\n";
    constexpr std::string_view geom    = "struct Point { i32 x; };\n"
                                         "i32 f( Point p ) { return p.x; }\n"
                                         "bool g() { return f( 5 ); }\n";

    SECTION( "inside the package, a bare call falls back" )
    {
        const Typed p(
            { { "main.kl", "import kl::geom;\ni32 main() { bool b = kl::g(); return 0; }\n" }, { "kl/geom.kl", geom } }, prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "from outside it, a qualified call does not" )
    {
        const Typed p(
            { { "main.kl", "import kl::geom;\ni32 main() { kl::f( 5 ); return 0; }\n" }, { "kl/geom.kl", geom } }, prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "Point" ) != std::string::npos );
        REQUIRE( p.chose( 0 ) == "geom:2" );
    }

    SECTION( "and a qualified call it does take is unchanged" )
    {
        const Typed p(
            { { "main.kl", "import kl::geom;\ni32 main() { return kl::f( kl::Point { 3 } ); }\n" }, { "kl/geom.kl", geom } },
            prelude
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.chose( 0 ) == "geom:2" );
    }
}

// D31: a borrow is the caller's variable itself, so there is no conversion for a widened copy to
// live in. One candidate is where this was missed, because selection never ran.
TEST_CASE( "a_borrowed_argument_takes_exactly_its_type", "[sema][overload][constref][out]" )
{
    SECTION( "`const ref`" )
    {
        const Typed p( "void f( const ref i64 a ) { }\ni32 main() { i32 b = 1; f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot borrow `i32` as `const ref i64`" ) != std::string::npos );
    }

    SECTION( "`const ref` of a temporary" )
    {
        const Typed p( "void f( const ref i64 a ) { }\ni32 main() { i32 b = 1; f( b + 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot borrow `i32` as `const ref i64`" ) != std::string::npos );
    }

    SECTION( "`out`" )
    {
        const Typed p( "void f( out i64 a ) { a = 1; }\ni32 main() { i32 b; f( out b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot borrow `i32` as `out i64`" ) != std::string::npos );
    }

    SECTION( "`ref`, as before" )
    {
        const Typed p( "void f( ref i64 a ) { }\ni32 main() { i32 b = 1; f( ref b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot borrow `i32` as `ref i64`" ) != std::string::npos );
    }

    SECTION( "through a function pointer" )
    {
        const Typed p( "void f( const ref i64 a ) { }\n"
                       "i32 main() { fn( const ref i64 ) -> void g = &f; i32 b = 1; g( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "cannot borrow `i32` as `const ref i64`" ) != std::string::npos );
    }

    // A type that does not convert at all is the argument check's error, and only that.
    SECTION( "a type that does not convert is reported once" )
    {
        const Typed p( "void f( const ref i64 a ) { }\ni32 main() { bool b = true; f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "expected `i64`, but got `bool`" ) != std::string::npos );
    }

    SECTION( "and so is one under `ref`" )
    {
        const Typed p( "void f( ref i64 a ) { }\ni32 main() { bool b = true; f( ref b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "expected `i64`, but got `bool`" ) != std::string::npos );
    }

    SECTION( "and through a function pointer" )
    {
        const Typed p( "void f( ref i64 a ) { }\n"
                       "i32 main() { fn( ref i64 ) -> void g = &f; bool b = true; g( ref b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "expected `i64`, but got `bool`" ) != std::string::npos );
    }

    SECTION( "a literal takes the borrow's type, so it is not refused" )
    {
        const Typed p( "void f( const ref i64 a ) { }\ni32 main() { f( 5 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a bare parameter still widens" )
    {
        const Typed p( "void f( i64 a ) { }\ni32 main() { i32 b = 1; f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// Two constructors are §12's narrowest slice and the one with a real limitation behind it: a class
// that can be built only one way.
TEST_CASE( "type_checker_chooses_between_constructors", "[sema][overload][aggregates]" )
{
    SECTION( "by the argument's type" )
    {
        const Typed p( "class C { i32 x; C( i32 v ) { x = v; } C( bool v ) { x = 0; } };\n"
                       "i32 main() { C a = C( 1 ); C b = C( true ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and the call records which one, for lowering to read back" )
    {
        Typed p( "class C { i32 x; C( i32 v ) { x = v; } C( bool v ) { x = 0; } };\n"
                 "i32 main() { C b = C( true ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );

        const Node_id chosen = p.types().callee_of( p.nth( Node_kind::Call_expr, 0 ) );

        REQUIRE( chosen == p.nth( Node_kind::Constructor_decl, 1 ) );
    }

    SECTION( "two with the same parameters are still refused" )
    {
        const Typed p( "class C { i32 x; C( i32 v ) { x = v; } C( i32 w ) { x = w; } };\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "already declared with these parameters" ) != std::string::npos );
    }
}

// A candidate list is a diagnostic about the instance the author named, so it spells that
// instance's parameters. The method half of this always did; the construction half read the
// declaration until the expectation was given to it.
TEST_CASE( "overloads_name_the_instance_a_candidate_list_is_about", "[sema][overload][generic]" )
{
    SECTION( "a constructor list takes its types from the expectation" )
    {
        const Typed p( "class Box<T> where T : Copyable\n"
                       "{\n"
                       "    T v;\n"
                       "    Box( T a ) { this.v = a; }\n"
                       "    Box( T a, T b ) { this.v = a; }\n"
                       "};\n"
                       "i32 main() { Box<i32> b = Box( ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "the ones declared take `( i32 )` and `( i32, i32 )`" ) != std::string::npos );
    }

    SECTION( "a method list takes them from the receiver" )
    {
        const Typed p( "struct Box<T> where T : Copyable\n"
                       "{\n"
                       "    T v;\n"
                       "    i32 pick( T a ) { return 0; }\n"
                       "    i32 pick( T a, T b ) { return 0; }\n"
                       "};\n"
                       "i32 main() { Box<i32> b = Box { 7 }; return b.pick( ); }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "the ones declared take `( i32 )` and `( i32, i32 )`" ) != std::string::npos );
    }

    SECTION( "the no-match list takes them too" )
    {
        const Typed p( "class Box<T> where T : Copyable\n"
                       "{\n"
                       "    T v;\n"
                       "    Box( T a, u8 b ) { this.v = a; }\n"
                       "    Box( T a, bool b ) { this.v = a; }\n"
                       "};\n"
                       "i32 main() { Box<i32> b = Box( 1, 1.5 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "no `Box` matches these arguments" ) != std::string::npos );
        REQUIRE( p.rendered().find( "the ones declared take `( i32, u8 )` and `( i32, bool )`" ) != std::string::npos );
    }

    SECTION( "and so does the ambiguity list" )
    {
        const Typed p( "class Box<T> where T : Copyable\n"
                       "{\n"
                       "    T v;\n"
                       "    Box( T a, u8 b ) { this.v = a; }\n"
                       "    Box( T a, u16 b ) { this.v = a; }\n"
                       "};\n"
                       "i32 main() { Box<i32> b = Box( 1, 2 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "this call to `Box` is ambiguous" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`( i32, u8 )` and `( i32, u16 )`" ) != std::string::npos );
    }

    SECTION( "with no instance to name, the declaration is what the author wrote" )
    {
        // Inside a generic the receiver really is `Box<U>`, so `U` is the honest spelling.
        const Typed p( "struct Box<T> where T : Copyable\n"
                       "{\n"
                       "    T v;\n"
                       "    i32 pick( T a ) { return 0; }\n"
                       "    i32 pick( T a, T b ) { return 0; }\n"
                       "};\n"
                       "i32 use<U>( Box<U> b ) where U : Copyable { return b.pick( ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "the ones declared take `( U )` and `( U, U )`" ) != std::string::npos );
    }
}

// The instance decides which constructor was meant, not only how the failure is spelled. A
// constructor's type parameters are the aggregate's, so `Box<i32>` settles `T` before an argument
// is read - the receiver's role in a method call rather than selection by what the result is for.
TEST_CASE( "overloads_choose_a_constructor_by_the_instance", "[sema][overload][generic][aggregates]" )
{
    SECTION( "a literal that cannot deduce `T` no longer rules every candidate out" )
    {
        const Typed p( "class Box<T> where T : Copyable\n"
                       "{\n"
                       "    T v;\n"
                       "    Box( T a, u8 b ) { this.v = a; }\n"
                       "    Box( T a, bool b ) { this.v = a; }\n"
                       "};\n"
                       "i32 main() { Box<i32> b = Box( 7, true ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and the one it chose is the one recorded" )
    {
        Typed p( "class Box<T> where T : Copyable\n"
                 "{\n"
                 "    T v;\n"
                 "    Box( T a, u8 b ) { this.v = a; }\n"
                 "    Box( T a, bool b ) { this.v = a; }\n"
                 "};\n"
                 "i32 main() { Box<i32> b = Box( 7, true ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.types().callee_of( p.nth( Node_kind::Call_expr, 0 ) ) == p.nth( Node_kind::Constructor_decl, 1 ) );
    }
}

// A method call is the same choice made from the receiver's members, and a bare name inside a
// method reaches the same set through the member scope.
TEST_CASE( "type_checker_chooses_between_methods", "[sema][overload][method]" )
{
    SECTION( "on the object" )
    {
        const Typed p( "class C { i32 x; C( i32 v ) { x = v; } i32 at( i32 n ) { return n; } "
                       "bool at( f64 n ) { return true; } };\n"
                       "i32 main() { C c = C( 1 ); bool b = c.at( 1.0 ); return c.at( 2 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and by bare name from a sibling" )
    {
        const Typed p( "class C { i32 x; C( i32 v ) { x = v; } i32 at( i32 n ) { return n; } "
                       "bool at( f64 n ) { return true; } bool use() { return at( 1.0 ); } };\n"
                       "i32 main() { C c = C( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a receiver's type arguments reach the comparison" )
    {
        // `put( T )` on a `S<bool>` is `put( bool )`, so the `bool` argument picks it rather than
        // the `i32` one - which is the receiver's bindings being applied before the types are
        // compared, not after.
        const Typed p( "class S<T> where T : Copyable { T v; S( T a ) { v = a; } "
                       "i32 put( T a ) { return 0; } i32 put( i32 a ) { return 1; } };\n"
                       "i32 main() { S<bool> s = S<bool>( true ); return s.put( true ); }" );

        INFO( p.rendered() );

        // Refused at the declaration, because `S<i32>` would make the two one signature.
        REQUIRE( p.rendered().find( "a type argument could make the two identical" ) != std::string::npos );
    }
}

// The marker and the signature must agree for `out` as for the rest, and one error rather than
// two - the marker is the only thing wrong here.
TEST_CASE( "type_checker_requires_the_out_marker_to_agree", "[sema][out]" )
{
    const Typed p( "void f( i32 x ) { }\ni32 main() { i32 a = 1; f( out a ); return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "does not assign this argument" ) != std::string::npos );
}

TEST_CASE( "type_checker_requires_the_ref_marker_at_the_call", "[sema][ref]" )
{
    constexpr std::string_view bump = "void bump( ref i32 n ) { n = n + 1; }\n";

    // D2: a reader of the call site should never have to find the declaration to learn that the
    // callee may write through the argument. Note the type owns nothing - unlike `move`, `ref` is
    // not exempt there, because it changes what the callee is holding whatever the type is.
    SECTION( "a bare argument is refused" )
    {
        const Typed p( std::string( bump ) + "i32 main() { i32 x = 1; bump( x ); return x; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "write `ref`" ) != std::string::npos );
    }

    SECTION( "a marker the signature does not ask for is refused" )
    {
        const Typed p( "void plain( i32 n ) { }\ni32 main() { i32 x = 1; plain( ref x ); return x; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "remove `ref`" ) != std::string::npos );
    }

    // The exemption that remains is `move` on a non-owning type: it is the caller's own assertion
    // that the source is dead afterwards, which the callee neither sees nor cares about.
    SECTION( "move on a non-owning type is still exempt" )
    {
        const Typed p( "void plain( i32 n ) { }\ni32 main() { i32 x = 1; plain( move x ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "with the marker it is accepted" )
    {
        const Typed p( std::string( bump ) + "i32 main() { i32 x = 1; bump( ref x ); return x; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// The marker at a call site announces that something happens to the caller's variable. With
// `const ref` nothing does - it is alive and unchanged afterwards, exactly like a bare argument -
// so by D31's own argument there is no marker to write.
TEST_CASE( "type_checker_takes_no_marker_for_a_const_ref", "[sema][constref]" )
{
    constexpr std::string_view peek = "struct P { i32 x; };\ni32 peek( const ref P p ) { return p.x; }\n";

    SECTION( "a bare argument is what it wants" )
    {
        const Typed p( std::string( peek ) + "i32 main() { P v = P { 1 }; return peek( v ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and `ref` at the call is refused" )
    {
        const Typed p( std::string( peek ) + "i32 main() { P v = P { 1 }; return peek( ref v ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "does not modify this argument" ) != std::string::npos );
    }

    // A const argument is exactly what a read-only borrow is for, and passing one must not be the
    // laundering the mutable borrow refuses.
    SECTION( "a const variable may be passed" )
    {
        const Typed p( std::string( peek ) + "i32 main() { const P v = P { 1 }; return peek( v ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The contrast that gives the case above its meaning.
    SECTION( "where a mutable borrow of the same variable is not" )
    {
        const Typed p( "struct P { i32 x; };\nvoid grow( ref P p ) { p.x = 1; }\n"
                       "i32 main() { const P v = P { 1 }; grow( ref v ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`v` is `const`" ) != std::string::npos );
    }
}

// §12: deduced from the argument types and, for what they leave open, from the expectation at the
// call site. Every case below reads its answer off a type that would not check if the deduction had
// gone the other way, rather than off a node index.
TEST_CASE( "type_checker_deduces_type_arguments", "[sema][generic]" )
{
    const std::string_view id  = "T id<T>( T a ) where T : Copyable { return a; }\n";
    const std::string_view box = "struct Box<T> where T : Copyable { T v; };\n";

    SECTION( "from an argument" )
    {
        // `bool` and `i32` widen into each other in neither direction, so each assignment checks
        // only if `T` came from the argument beside it.
        const Typed p(
            std::string( id ) + "i32 main() { bool t = true; bool b = id( t ); i32 n = 1; i32 m = id( n ); "
                                "return m + 0 * n; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "structurally, not positionally" )
    {
        const Typed p(
            std::string( box ) + "T first<T>( Box<T> b ) where T : Copyable { return b.v; }\n"
                                 "i32 main() { Box<bool> b = Box { true }; bool t = first( b ); return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "from the expectation, when the parameter is only in the result" )
    {
        const Typed p( "T zeroed<T>() where T : Integral { return wrap<T>( 0 ); }\n"
                       "i32 main() { i64 wide = zeroed(); return 0 * wrap<i32>( wide ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The two sources are not equal. `i64 x = id( n );` on an `i32` instantiates `id<i32>` and
    // widens the result, exactly as the written `id<i32>( n )` does - an expectation is where the
    // value is going, not a claim about how it was made.
    SECTION( "an argument beats the expectation" )
    {
        const Typed p( std::string( id ) + "i32 main() { i32 n = 1; i64 wide = id( n ); return 0 * wrap<i32>( wide ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and the expectation cannot retype the argument" )
    {
        const Typed p( std::string( id ) + "i32 main() { i32 n = 1; bool b = id( n ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "expected `bool`, but got `i32`" ) != std::string::npos );
    }

    // D26 again: the context that would give a literal its type is the thing being deduced.
    SECTION( "a literal says nothing, and a real argument beside it decides" )
    {
        const Typed p( "T both<T>( T a, T b ) where T : Copyable { return a; }\n"
                       "i32 main() { i64 wide = 1; i64 answer = both( wide, 1 ); return 0 * wrap<i32>( answer ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "two arguments that disagree name the disagreement" )
    {
        const Typed p( "T both<T>( T a, T b ) where T : Copyable { return a; }\n"
                       "i32 main() { i32 n = 1; bool t = true; both( n, t ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`T` cannot be both `i32` and `bool`" ) != std::string::npos );
    }

    // All or nothing: a partial list is a rule nobody can recite, so one parameter left open sends
    // the whole call to the explicit form.
    SECTION( "one parameter deduced and one not is still a failure" )
    {
        const Typed p( "U second<T, U>( T a ) where T : Copyable, where U : Integral { return wrap<U>( 0 ); }\n"
                       "i32 main() { i32 n = 1; second( n ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "nothing here says what `U` is in `second`" ) != std::string::npos );
    }

    // Deduction reads an argument without judging it, so the one that deduced nothing still has to
    // be checked against the parameter once the others have settled it.
    SECTION( "an argument deduction could not read is still checked" )
    {
        const Typed p(
            std::string( box ) + "T first<T>( Box<T> b ) where T : Copyable { return b.v; }\n"
                                 "i32 main() { i32 n = 1; return first( n ); }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "expected `Box<i32>`, but got `i32`" ) != std::string::npos );
    }

    // A bound is a promise about the type argument however it was arrived at, and what underlines
    // it is the argument that chose the type - there is no written annotation to point at.
    SECTION( "a bound still holds, and blames the argument that chose the type" )
    {
        const Typed p( "struct Plain { i32 v; };\ni32 ordered<T>( T a ) where T : Comparable { return 1; }\n"
                       "i32 main() { Plain p = Plain { 1 }; return ordered( p ); }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`Plain` is not `Comparable`" ) != std::string::npos );
    }

    // §12's tie-break, unreachable until a call could leave its type arguments unwritten: it is the
    // thing that puts a generic and a non-generic in one candidate set.
    SECTION( "a non-generic candidate beats a generic one" )
    {
        const Typed p( "bool f( i32 a ) { return true; }\nT f<T>( T a ) where T : Copyable { return a; }\n"
                       "i32 main() { i32 n = 1; bool b = f( n ); i32 m = f<i32>( n ); return m; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and two generics are ambiguous rather than ranked" )
    {
        const Typed p(
            std::string( box ) + "bool which<T>( T a ) where T : Copyable { return true; }\n"
                                 "bool which<T>( Box<T> a ) where T : Copyable { return false; }\n"
                                 "i32 main() { Box<i32> b = Box { 1 }; which( b ); return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "is ambiguous" ) != std::string::npos );
        REQUIRE( p.rendered().find( "write the type arguments" ) != std::string::npos );
    }

    // Selection by arity alone still leaves the parameters to deduce: it reaches an answer before
    // any argument has been typed, so the deduction cannot live inside the choosing.
    SECTION( "a candidate chosen by arity is still deduced" )
    {
        const Typed p( "T one<T>( T a ) where T : Copyable { return a; }\n"
                       "T one<T>( T a, T b ) where T : Copyable { return b; }\n"
                       "i32 main() { bool t = true; bool b = one( t, t ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // A constructor produces the aggregate rather than returning anything, so the instance comes
    // from the expectation exactly as a struct literal's does.
    SECTION( "a constructor takes its instance from the expectation" )
    {
        const Typed p( "class Held<T> where T : Copyable { T v; Held( T value ) { v = value; } "
                       "T get() const { return v; } };\n"
                       "i32 main() { Held<bool> h = Held( true ); bool t = h.get(); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // The expectation belongs to the call it was written for and to nothing inside it. Without
    // that, an argument would take the type its *parent's* result is wanted at, which is only ever
    // right by accident - so a call with nothing of its own to read is refused and says so.
    SECTION( "an expectation does not reach into an argument" )
    {
        const Typed p(
            std::string( id ) + "T zeroed<T>() where T : Integral { return wrap<T>( 0 ); }\n"
                                "i32 main() { i64 wide = id( zeroed() ); return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "nothing here says what `T` is in `zeroed`" ) != std::string::npos );
    }

    SECTION( "a deduced call and a written one are one instantiation" )
    {
        const Typed p( std::string( id ) + "i32 main() { i32 n = 1; return id( n ) + id<i32>( n ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.types().instantiations().size() == 1 );
    }
}

// The other half: pairs no call site could ever tell apart, refused where they are written.
TEST_CASE( "type_checker_refuses_overloads_nothing_can_tell_apart", "[sema][overload]" )
{
    SECTION( "the same parameters twice" )
    {
        const Typed p( "void f( i32 a ) { }\nvoid f( i32 b ) { }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "already declared with these parameters" ) != std::string::npos );
    }

    SECTION( "differing only in the return type" )
    {
        const Typed p( "void f( i32 a ) { }\ni32 f( i32 a ) { return a; }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "not only in what they return" ) != std::string::npos );
    }

    // A `const ref` takes no marker at the call, which is the whole reason it cannot sit beside a
    // bare parameter - and also why the two are allowed to share a mangled name.
    SECTION( "a `const ref` beside a bare parameter" )
    {
        const Typed p( "void f( i32 a ) { }\nvoid f( const ref i32 a ) { }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "already declared with these parameters" ) != std::string::npos );
    }

    SECTION( "two methods differing only in a trailing `const`" )
    {
        const Typed p( "class C { i32 x; C( i32 v ) { x = v; } i32 n() { return x; } i32 n() const { return x; } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`const` binds the receiver" ) != std::string::npos );
    }

    // D11: a generic that checks must instantiate. A method call writes no type argument, so a pair
    // some instantiation would make identical is refused here rather than at that instantiation.
    SECTION( "two members a type argument could make identical" )
    {
        const Typed p( "class S<T> where T : Copyable { T v; S( T a ) { v = a; } "
                       "i32 f( T a ) { return 0; } i32 f( i32 a ) { return 1; } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "a type argument could make the two identical" ) != std::string::npos );
    }

    SECTION( "but two that no argument could" )
    {
        const Typed p( "class S<T> where T : Copyable { T v; S( T a ) { v = a; } "
                       "i32 f( T a ) { return 0; } i32 f( T a, i32 b ) { return b; } };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "an `extern`, which keeps the name someone else defined" )
    {
        const Typed p( "extern void f( i32 a );\nextern void f( f64 a );\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "defined in C, so it cannot be overloaded" ) != std::string::npos );
    }

    SECTION( "a second `main`" )
    {
        const Typed p( "i32 main() { return 0; }\ni32 main( i32 a ) { return a; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "a program has one `main`" ) != std::string::npos );
    }

    // Only two callables of one kind are a set. Everything else is still a redeclaration, and the
    // resolver still says so.
    SECTION( "a function beside a variable is not an overload set" )
    {
        const Typed p( "void f( i32 a ) { }\ni32 f = 1;\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
        REQUIRE( p.rendered().find( "already declared" ) != std::string::npos );
    }
}

// The arity diagnostic says what is wrong with the call; it does not excuse the arguments from
// being read. An extra argument is nobody's parameter, so nothing else will ever look inside it.
TEST_CASE( "overloads_still_walk_an_argument_no_parameter_claims", "[sema][overload]" )
{
    const Typed p( "i32 g( i32 a ) { return a; }\ni32 main() { bool b = true; return g( 1, b + 1 ); }" );

    INFO( p.rendered() );
    REQUIRE( p.rendered().find( "takes 1 argument, but 2 were given" ) != std::string::npos );
    REQUIRE( p.rendered().find( "no operator `+` for `bool` and `bool`" ) != std::string::npos );
}

// Every refusal of a pair points at the one it collides with. `main` is the branch that is easiest
// to write without it, because the second declaration reads as complete on its own.
TEST_CASE( "overloads_name_the_declaration_a_duplicate_collides_with", "[sema][overload]" )
{
    const Typed p( "i32 main() { return 0; }\ni32 main() { return 1; }" );

    INFO( p.rendered() );
    REQUIRE( p.errors() == 1 );
    REQUIRE( p.rendered().find( "a program has one `main`" ) != std::string::npos );
    REQUIRE( p.rendered().find( "previous declaration is at: t.kl:1:1" ) != std::string::npos );
}

// A refused duplicate leaves the set, so the first declaration answers every use of the name and
// one mistake is one error.
TEST_CASE( "overloads_absorb_a_refused_duplicate", "[sema][overload]" )
{
    SECTION( "a call to a function declared twice" )
    {
        const Typed p( "i32 f( i32 a ) { return a; }\ni32 f( i32 b ) { return b; }\ni32 main() { return f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.types().callee_of( p.nth( Node_kind::Call_expr, 0 ) ) == p.nth( Node_kind::Function_decl, 0 ) );
    }

    SECTION( "three of one signature are two mistakes" )
    {
        const Typed p( "i32 f( i32 a ) { return a; }\ni32 f( i32 b ) { return b; }\ni32 f( i32 c ) { return c; }\n"
                       "i32 main() { return f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 2 );
        REQUIRE( p.rendered().find( "previous declaration is at: t.kl:2:1" ) == std::string::npos );
    }

    SECTION( "one differing only in what it returns" )
    {
        const Typed p( "void f( i32 a ) { }\ni32 f( i32 a ) { return a; }\ni32 main() { f( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.types().callee_of( p.nth( Node_kind::Call_expr, 0 ) ) == p.nth( Node_kind::Function_decl, 0 ) );
    }

    SECTION( "its address" )
    {
        const Typed p( "i32 f( i32 a ) { return a; }\ni32 f( i32 b ) { return b; }\n"
                       "i32 main() { auto p = &f; return p( 2 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
    }

    SECTION( "a constructor" )
    {
        const Typed p( "class C { i32 x; C( i32 v ) { x = v; } C( i32 w ) { x = w; } };\n"
                       "i32 main() { C c = C( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.types().callee_of( p.nth( Node_kind::Call_expr, 0 ) ) == p.nth( Node_kind::Constructor_decl, 0 ) );
    }

    SECTION( "a method, called on an object and by bare name" )
    {
        const Typed p( "class C { i32 x; C( i32 v ) { x = v; } i32 n() { return x; } i32 n() { return x; } "
                       "i32 m() { return n(); } };\n"
                       "i32 main() { C c = C( 1 ); return c.n(); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.types().callee_of( p.nth( Node_kind::Call_expr, 2 ) ) == p.nth( Node_kind::Method_decl, 0 ) );
    }
}

// D31's exemption is what lets a `move` on a copyable type coexist with a `const ref` that takes
// the same type: the marker selects, except where it says nothing the callee could see.
TEST_CASE( "overloads_exempt_move_on_a_type_that_owns_nothing", "[sema][overload][move]" )
{
    const Typed p( "class Owned { public i32 v; ~Owned() { } };\n"
                   "i32 by_transfer( const ref Owned a ) { return 1; }\n"
                   "i32 by_transfer( move Owned a ) { return 2; }\n"
                   "i32 by_copy( i32 a ) { return 3; }\n"
                   "i32 by_copy( bool a ) { return 4; }\n"
                   "i32 main() { Owned g = Owned { 1 }; i32 n = 1; return by_transfer( move g ) + by_copy( move n ); }" );

    INFO( p.rendered() );
    REQUIRE( p.clean() );
}

// A bound broken by a deduced type argument underlines the argument that chose it, not the call:
// `T` is not something the author wrote, so the call is not where the mistake is.
TEST_CASE( "overloads_underline_the_argument_that_chose_the_type", "[sema][generic][bound]" )
{
    const Typed p( "struct Plain { i32 v; };\n"
                   "i32 ordered<T>( T a ) where T : Comparable { return 1; }\n"
                   "i32 breaks( Plain p ) { return ordered( p ); }\n"
                   "i32 main() { return 0; }" );

    INFO( p.rendered() );
    REQUIRE( p.rendered().find( "^ ordering needs a number" ) != std::string::npos );

    // One caret, so it is the argument and not the seven characters of `ordered`.
    REQUIRE( p.rendered().find( "^^ ordering needs a number" ) == std::string::npos );
}

// PLAN §6.7. A member's parameter 0 is the receiver and the overload check drops it before
// comparing, which is what makes `area()` and `area() const` one signature. A static method has no
// parameter 0 to drop, so a check that decides how many to skip from the *node kind* compares the
// wrong lists - and the pair it then fails to separate emit one C symbol, which is the collision
// M6.5 paid off for methods and free functions.
TEST_CASE( "overloads_tell_static_methods_apart_by_their_written_parameters", "[sema][static][overload]" )
{
    SECTION( "two that differ in the first parameter are two" )
    {
        const Typed p( "struct P { i32 x; static i32 f( i32 a ) { return a; } static i32 f( bool a ) { return 1; } };\n"
                       "i32 main() { return P::f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and two that agree on every one are refused" )
    {
        const Typed p( "struct P { i32 x; static i32 f( i32 a ) { return a; } static i32 f( i32 b ) { return 1; } };\n"
                       "i32 main() { return P::f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    SECTION( "one taking nothing is not the same as one taking something" )
    {
        const Typed p( "struct P { i32 x; static i32 f() { return 1; } static i32 f( i32 a ) { return a; } };\n"
                       "i32 main() { return P::f(); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A static and an instance method of one name take the same written parameters and mangle to one
// symbol, because the tag names the enclosing type and the receiver is not among the parameters
// either way. The call spellings differ, so nothing at a call site is ambiguous - but `cc` refuses
// two definitions of one name, which is a failure in generated code rather than a diagnostic.
TEST_CASE( "overloads_refuse_a_static_and_an_instance_method_of_one_signature", "[sema][static][overload]" )
{
    SECTION( "same name, same written parameters" )
    {
        const Typed p( "struct P { i32 x; static i32 f( i32 a ) { return a; } i32 f( i32 a ) const { return a + x; } };\n"
                       "i32 main() { return P::f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // Declaration order must not decide it: the pair is checked once per ordered pair, and the
    // instance-first spelling is the one a check keyed off the first declaration would miss.
    SECTION( "and the other way round" )
    {
        const Typed p( "struct P { i32 x; i32 f( i32 a ) const { return a + x; } static i32 f( i32 a ) { return a; } };\n"
                       "i32 main() { return P::f( 1 ); }" );

        INFO( p.rendered() );
        REQUIRE_FALSE( p.clean() );
    }

    // Different parameters are genuinely two functions and mangle apart, so this is the boundary
    // rather than a blanket refusal of the two kinds sharing a name.
    SECTION( "but differing parameters are two functions" )
    {
        const Typed p( "struct P { i32 x; static i32 f( bool a ) { return 1; } i32 f( i32 a ) const { return a + x; } };\n"
                       "i32 main() { return P::f( true ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

} // namespace keel
#endif
