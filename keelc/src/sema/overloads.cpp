#include "sema/overloads.h"

#include <fmt/format.h>

#include <algorithm>
#include <unordered_map>

// Overloading: which callable a name means at a call site, what its type arguments are, and
// whether two declarations of one name could ever be told apart.

namespace keel
{
namespace sema
{

namespace
{
// D31's markers are part of what a call site writes, so this is the whole of what a call can say
// about one parameter. `const ref T` takes no marker, which is exactly why it cannot be told from a
// bare `T` - and `ref T` and `T*` differ here, which is why they can coexist.
Keyword call_marker( const Ast& ast, Node_id param )
{
    return is_const_binding( ast, param ) ? Keyword::Count : parameter_mode( ast, param );
}

} // namespace

// The types the call wrote, mapped onto the callable's own type parameters. Positional, because
// that is the only correspondence there is - `f<T>` and `f<U>` name theirs differently.
Bindings Overloads::type_bindings( Node_id callable, std::span<const Type_id> arguments ) const
{
    const std::vector<Node_id> parameters = type_parameters( ast_, ast_.type_param_list( callable ) );

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
    const std::span<const Node_id> params = ast_.children( ast_.child( callable, 1 ) ).subspan( implicit_params );

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

    for( const Node_id parameter : type_parameters( ast_, ast_.type_param_list( callable ) ) )
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
    const std::vector<Node_id>     parameters = type_parameters( ast_, ast_.type_param_list( callable ) );
    const std::span<const Node_id> params     = ast_.children( ast_.child( callable, 1 ) ).subspan( implicit_params );

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

            if( already->second == found->second )
            {
                continue;
            }

            reporter_.error_at(
                ast_.span( arguments[i] ),
                fmt::format(
                    "`{}` cannot be both `{}` and `{}`",
                    interner_.text( Symbol_id { ast_.aux( parameter ) } ),
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
                fmt::format(
                    "nothing here says what `{}` is in `{}`", interner_.text( Symbol_id { ast_.aux( parameter ) } ), name
                ),
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
    const Keyword wanted = call_marker( ast_, param );

    if( wanted == given )
    {
        return true;
    }

    const bool about_ownership =
        ( wanted == Keyword::Count || wanted == Keyword::Move ) && ( given == Keyword::Count || given == Keyword::Move );

    return about_ownership && bounds_.satisfies( expected, Bound::Copyable );
}

// Exactly, or not at all. §6.4's widening is what gets an argument to a parameter *after* one has
// been chosen; letting it choose would mean ranking two parameters that both accept, which is the
// machinery D5 deleted.
bool Overloads::candidate_accepts(
    Node_id callable, u32 implicit_params, std::span<const Argument_shape> shapes, const Bindings& bindings
)
{
    const std::span<const Node_id> params = ast_.children( ast_.child( callable, 1 ) ).subspan( implicit_params );

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
            if( shapes[i].type != expected )
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

// The set narrowed by what the call wrote. Empty with a diagnostic already reported, one when that
// settled it, or the rest for the caller to shape its arguments for. Only ever reached with two or
// more candidates: one is the ordinary path, which this must leave exactly as it was.
std::vector<Node_id> Overloads::viable_overloads(
    Node_id call, std::string_view name, std::span<const Node_id> candidates, u32 implicit_params, Type_id instance
)
{
    const std::span<const Node_id> arguments = ast_.children( ast_.child( call, 1 ) );
    const Node_id                  type_args = ast_.child( call, 2 );
    const std::size_t              written   = type_args.is_valid() ? ast_.children( type_args ).size() : 0;

    // How many arguments and how many type arguments are what the call *says*, so they narrow the
    // set before anything is typed - and typing an argument is what cannot be taken back.
    std::vector<Node_id> viable;

    for( const Node_id candidate : candidates )
    {
        const std::size_t params = ast_.children( ast_.child( candidate, 1 ) ).size() - implicit_params;

        // A call that wrote type arguments means them, so a candidate has to take exactly that many
        // - which keeps a non-generic out of `f<i32>( x )`. A call that wrote none says nothing
        // about genericity, so both kinds stay and the types decide below.
        if( params == arguments.size() &&
            ( written == 0 || type_parameters( ast_, ast_.type_param_list( candidate ) ).size() == written ) )
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
            [&]( Node_id candidate )
            { return ast_.children( ast_.child( candidate, 1 ) ).size() - implicit_params == arguments.size(); }
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
    const Node_id     type_args = ast_.child( call, 2 );
    const std::size_t written   = type_args.is_valid() ? ast_.children( type_args ).size() : 0;

    if( type_args.is_valid() )
    {
        for( const Node_id written_argument : ast_.children( type_args ) )
        {
            resolved.push_back( annotations_.type_of( written_argument ) );
        }
    }

    // A construction's type parameters are the aggregate's, and the instance already says what they
    // are - so `Box<i32> b = Box( 7, true );` settles `T` before an argument is read. That is the
    // receiver's role in select_method rather than selection by return type, which
    // deduce_for_candidate refuses for a function and still refuses here.
    const Bindings from_instance = aggregates_.bindings_of( instance );

    std::vector<Node_id> matching;

    for( const Node_id candidate : viable )
    {
        // Deduced only to ask whether this one could have been meant. What it found is thrown
        // away: the caller deduces again once the callable is settled, with the expectation in
        // hand and with a diagnostic to give, and keeping it here would be two answers to keep
        // in step.
        std::vector<Type_id> deduced;
        const bool           infers = written == 0 && from_instance.empty() && is_generic( ast_, candidate );

        if( infers && !deduce_for_candidate( candidate, implicit_params, shapes, deduced ) )
        {
            continue; // nothing here says what its parameters are, so it is not what was meant
        }

        // What the call wrote or the arguments deduced wins; the instance fills what is left.
        Bindings bindings = type_bindings( candidate, infers ? deduced : resolved );
        bindings.insert( from_instance.begin(), from_instance.end() );

        if( candidate_accepts( candidate, implicit_params, shapes, bindings ) )
        {
            matching.push_back( candidate );
        }
    }

    // §12: a non-generic candidate beats a generic one, and two generics are ambiguous. It is one
    // rule read off the declarations rather than a ranking of how well each fits, which is the
    // distinction the whole feature rests on. Unreachable until the call could leave its type
    // arguments unwritten, which is what puts both kinds in one set.
    if( matching.size() > 1 )
    {
        Node_id     concrete {};
        std::size_t found = 0;

        for( const Node_id candidate : matching )
        {
            if( !is_generic( ast_, candidate ) )
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

    if( matching.size() == 1 )
    {
        return matching.front();
    }

    if( matching.empty() )
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
    // so it cannot choose between two parameters in one family. Where every one left is a generic
    // the arguments cannot say it either - both fit exactly - and only the type arguments can.
    const bool all_generic =
        std::all_of( matching.begin(), matching.end(), [&]( Node_id candidate ) { return is_generic( ast_, candidate ); } );

    reporter_.error_at(
        ast_.span( call ),
        fmt::format( "this call to `{}` is ambiguous", name ),
        fmt::format(
            "more than one matches: {}; {}",
            candidate_list( matching, implicit_params, aggregates_.bindings_of( instance ) ),
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

    for( Node_id candidate = first; candidate.is_valid(); candidate = resolution_.next_overload( candidate ) )
    {
        candidates.push_back( candidate );
    }

    if( candidates.size() == 1 )
    {
        return candidates; // one is the ordinary path, and the arity below is check_call_arguments'
    }

    const std::span<const Node_id> arguments = ast_.children( ast_.child( call, 1 ) );
    const std::string_view         name      = interner_.text( Symbol_id { ast_.aux( first ) } );
    const Bindings                 bindings  = aggregates_.bindings_of( receiver );

    std::vector<Node_id> viable;

    for( const Node_id candidate : candidates )
    {
        if( ast_.children( ast_.child( candidate, 1 ) ).size() - 1 == arguments.size() )
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
    const std::string_view name     = interner_.text( Symbol_id { ast_.aux( first ) } );
    const Bindings         bindings = aggregates_.bindings_of( receiver );

    std::vector<Node_id> matching;

    for( const Node_id candidate : viable )
    {
        if( candidate_accepts( candidate, 1, shapes, bindings ) )
        {
            matching.push_back( candidate );
        }
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
    const std::span<const Node_id> declared  = ast_.children( ast_.child( callable, 1 ) );
    const std::span<const Node_id> params    = declared.subspan( implicit_params );
    const std::span<const Node_id> arguments = ast_.children( ast_.child( call, 1 ) );

    std::vector<Argument_work> work;

    if( params.size() != arguments.size() )
    {
        reporter_.error_at(
            ast_.span( ast_.child( call, 1 ) ),
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
    const std::span<const Node_id> params    = ast_.children( ast_.child( callable, 1 ) ).subspan( implicit_params );
    const std::span<const Node_id> arguments = ast_.children( ast_.child( call, 1 ) );

    for( std::size_t i = 0; i < std::min( params.size(), arguments.size() ); ++i )
    {
        const Type_id expected = table_.substitute( types_.type_of( params[i] ), bindings );

        // D2: transfer is visible at the call *and* in the signature, and neither alone is enough -
        // a reader of one should never have to find the other.
        // What a marker announces is that something happens to the caller's variable. `const ref` is
        // the one mode where nothing does - alive and unchanged afterwards, exactly like a bare
        // argument - so it sits with bare rather than with `ref`, and takes no marker at the call.
        const Keyword wanted = is_const_binding( ast_, params[i] ) ? Keyword::Count : parameter_mode( ast_, params[i] );
        const Keyword given  = ast_.kind( arguments[i] ) == Node_kind::Marker_expr
                                   ? static_cast<Keyword>( ast_.aux( arguments[i] ) )
                                   : Keyword::Count;

        // A ref binds to the caller's object itself, so there is no conversion step for a widened
        // copy to live in: `ref u8` and `ref i32` are different bindings, not convertible ones.
        // Above the agreement check, because agreeing is this rule's precondition rather than its
        // exit - and it wants both sides, so one missing marker does not report twice.
        if( wanted == Keyword::Ref && given == Keyword::Ref && !table_.is_error( types_.type_of( arguments[i] ) ) &&
            types_.type_of( arguments[i] ) != expected )
        {
            reporter_.error_at(
                ast_.span( arguments[i] ),
                fmt::format(
                    "cannot borrow `{}` as `ref {}`", table_.name( types_.type_of( arguments[i] ) ), table_.name( expected )
                ),
                "a borrow is the variable itself, so its type must match exactly"
            );
        }

        if( wanted == given )
        {
            continue;
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
            continue;
        }

        if( wanted == Keyword::Ref )
        {
            reporter_.error_at(
                ast_.span( arguments[i] ), fmt::format( "`{}` may modify this argument", name ), "write `ref`"
            );
        }
        else if( given == Keyword::Ref )
        {
            reporter_.error_at(
                ast_.span( arguments[i] ), fmt::format( "`{}` does not modify this argument", name ), "remove `ref`"
            );
        }
        else if( wanted == Keyword::Move )
        {
            reporter_.error_at(
                ast_.span( arguments[i] ), fmt::format( "`{}` takes ownership of this argument", name ), "write `move`"
            );
        }
        else if( given == Keyword::Move )
        {
            reporter_.error_at( ast_.span( arguments[i] ), fmt::format( "`{}` borrows this argument", name ), "remove `move`" );
        }
        else if( wanted == Keyword::Out )
        {
            reporter_.error_at( ast_.span( arguments[i] ), fmt::format( "`{}` assigns this argument", name ), "write `out`" );
        }
        else if( given == Keyword::Out )
        {
            reporter_.error_at(
                ast_.span( arguments[i] ), fmt::format( "`{}` does not assign this argument", name ), "remove `out`"
            );
        }
    }
}

bool Overloads::parameters_collide( Node_id first, Node_id second, bool member )
{
    // A member's parameter 0 is the receiver, which the call site never writes - so two that differ
    // only in it, `area()` and `area() const`, are one signature and have to be refused here. Asked
    // of each signature rather than of the kind, and separately: M7's static method is a member with
    // no receiver, so a pair may legitimately disagree about whether there is one to skip.
    const std::span<const Node_id> mine =
        ast_.children( ast_.child( first, 1 ) ).subspan( has_receiver( ast_, first ) ? 1 : 0 );
    const std::span<const Node_id> theirs =
        ast_.children( ast_.child( second, 1 ) ).subspan( has_receiver( ast_, second ) ? 1 : 0 );

    if( mine.size() != theirs.size() )
    {
        return false;
    }

    const std::vector<Node_id> my_parameters    = type_parameters( ast_, ast_.type_param_list( first ) );
    const std::vector<Node_id> their_parameters = type_parameters( ast_, ast_.type_param_list( second ) );

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
        if( call_marker( ast_, mine[i] ) != call_marker( ast_, theirs[i] ) )
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
    const std::string_view name = interner_.text( Symbol_id { ast_.aux( second ) } );

    // An extern names a symbol someone else defined, and `main` is the program's entry point:
    // both keep their spelling in C, so a second of either has nowhere to differ.
    if( is_extern( ast_, first ) || is_extern( ast_, second ) )
    {
        reporter_.error_at(
            ast_.span( second ),
            fmt::format( "`{}` is defined in C, so it cannot be overloaded", name ),
            reporter_.previous_declaration_note( ast_.span( first ) )
        );

        return;
    }

    if( ast_.kind( second ) == Node_kind::Function_decl && name == "main" )
    {
        reporter_.error_at(
            ast_.span( second ), "a program has one `main`", reporter_.previous_declaration_note( ast_.span( first ) )
        );

        return;
    }

    const bool exact = parameters_collide( first, second, false );

    if( !exact )
    {
        if( !member || !parameters_collide( first, second, true ) )
        {
            return;
        }

        reporter_.error_at(
            ast_.span( second ),
            fmt::format( "`{}` is already declared, and a type argument could make the two identical", name ),
            reporter_.previous_declaration_note( ast_.span( first ) )
        );

        return;
    }

    // Two declarations that differ only in what they return. Worth its own message: the author
    // wrote a difference, and it is not one a call site can act on.
    if( types_.type_of( first ) != types_.type_of( second ) )
    {
        reporter_.error_at(
            ast_.span( second ),
            fmt::format( "`{}` is already declared with these parameters", name ),
            "two of one name must differ in their parameters, not only in what they return"
        );

        return;
    }

    // M7: one takes an object and the other does not. The call spellings differ, so nothing at a
    // call site is ambiguous - but the category tag names the enclosing type either way and the
    // receiver is among the parameters in neither, so the two emit one symbol. Reported before the
    // `const` clause below, which would otherwise blame a keyword whose removal changes nothing.
    if( member && has_receiver( ast_, first ) != has_receiver( ast_, second ) )
    {
        reporter_.error_at(
            ast_.span( second ),
            fmt::format( "`{}` is already declared with these parameters", name ),
            "a `static` method and a method of one name must differ in their parameters"
        );

        return;
    }

    // And the same for a trailing `const`, which is a habit worth naming: it binds the receiver,
    // and a call writes the object rather than how the method holds it.
    if( member && is_const_method( ast_, first ) != is_const_method( ast_, second ) )
    {
        reporter_.error_at(
            ast_.span( second ),
            fmt::format( "`{}` is already declared with these parameters", name ),
            "`const` binds the receiver, which a call site does not write, so it cannot tell two apart"
        );

        return;
    }

    reporter_.error_at(
        ast_.span( second ),
        fmt::format( "`{}` is already declared with these parameters", name ),
        reporter_.previous_declaration_note( ast_.span( first ) )
    );
}

// Every ordered pair, once: the walk starts from each member of a chain and visits only what
// follows it.
void Overloads::check_overload_sets()
{
    for( const Node_id decl : ast_.children( ast_.root() ) )
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

        if( ast_.kind( member ) != Node_kind::Method_decl )
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
    const std::span<const Node_id> params = ast_.children( ast_.child( callable, 1 ) ).subspan( implicit_params );

    // A list of candidates can hold a generic nothing has instantiated - the call wrote the wrong
    // number of type arguments, or none - and substituting a `T` the map has no entry for asserts.
    // Unbound, the declaration is what the author wrote and is what the message should show.
    bool bound = true;

    for( const Node_id parameter : type_parameters( ast_, ast_.type_param_list( callable ) ) )
    {
        bound = bound && bindings.contains( types_.type_of( parameter ).v );
    }

    std::string text = "(";

    for( std::size_t i = 0; i < params.size(); ++i )
    {
        const Keyword marker = call_marker( ast_, params[i] );

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

} // namespace sema
} // namespace keel

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

    // The rule that keeps §12's promise of no ranking: widening is what gets an argument to a
    // parameter *after* one has been chosen, and two parameters that both accept would need one.
    SECTION( "selection is by exact type, not by what §6.4 would widen" )
    {
        const Typed p( "void f( i32 a ) { }\nvoid f( i64 a ) { }\n"
                       "i32 main() { u8 b = 1; f( b ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "no `f` matches these arguments" ) != std::string::npos );
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
    REQUIRE( p.rendered().find( "previous declaration is at: 1:1" ) != std::string::npos );
}

// D31's exemption is what lets a `move` on a copyable type coexist with a `const ref` that takes
// the same type: the marker selects, except where it says nothing the callee could see.
TEST_CASE( "overloads_exempt_move_on_a_type_that_owns_nothing", "[sema][overload][move]" )
{
    const Typed p( "class Owned { i32 v; ~Owned() { } };\n"
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

// PLAN §12, M7. A member's parameter 0 is the receiver and the overload check drops it before
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
