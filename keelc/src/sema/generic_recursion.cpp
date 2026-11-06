// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sema/generic_recursion.h"
#include <fmt/format.h>
#include <algorithm>
#include <optional>

namespace keel::sema
{

// An edge exists only between generics: a call in `main` is already an instance rather than a step
// towards one, so the caller is the guard rather than the call site. A global's initialiser has none.
void Generic_recursion::record_call( Node_id from, Node_id to, std::vector<Type_id> arguments, Span at )
{
    // D77: a default runs wherever its aggregate is built, so its calls are the aggregate's.
    if( from.is_valid() && ast_.kind( from ) == Node_kind::Field_decl )
    {
        from = ast_.enclosing_aggregate( from );
    }

    if( from.is_valid() && ast_.is_generic( from ) )
    {
        for( const Type_id argument : arguments )
        {
            record_uses( from, argument, at, implied_ ); // `count<Grow<T>>()` makes a `Grow<T>`
        }

        generic_calls_.push_back( Generic_call { .from = from, .to = to, .arguments = std::move( arguments ), .at = at } );
    }
    else
    {
        record_size_roots( to, arguments, at );
    }
}

// A type a generic body names is an instance it makes, as a call is: `Grow<Grow<T>>* p;` in
// `~Grow` makes a bigger `Grow` per instance. And each instance runs its destructor.
void Generic_recursion::record_bodies( Node_id id, Node_id from, std::span<const Type_id> types )
{
    const Node_kind kind = ast_.kind( id );

    if( is_function_like( kind ) || kind == Node_kind::Field_decl )
    {
        from = kind == Node_kind::Field_decl ? ast_.enclosing_aggregate( id ) : id;
        from = from.is_valid() && ast_.is_generic( from ) ? from : Node_id {};
        seen_.clear();

        if( from.is_valid() && kind == Node_kind::Destructor_decl )
        {
            implied_.push_back(
                Generic_call { .from = ast_.enclosing_aggregate( id ), .to = id, .arguments = {}, .at = ast_.span( id ) }
            );
        }
    }

    if( id.v < types.size() && types[id.v].is_valid() && seen_.insert( types[id.v].v ).second )
    {
        if( from.is_valid() )
        {
            record_uses( from, types[id.v], ast_.span( id ), implied_ );
        }
        else
        {
            record_size_roots( Node_id {}, std::span( &types[id.v], 1 ), ast_.span( id ) );
        }
    }

    for( const Node_id child : ast_.children( id ) )
    {
        if( child.is_valid() )
        {
            record_bodies( child, from, types );
        }
    }
}

// A field naming another generic is the same edge a call is, so it goes in the same graph: a cycle
// through an aggregate and a function is then found the way one through two functions is.
//
// `Bad<Box<T>>` is two facts, so the arguments are walked as well as the type. No visited-set - an
// interned type is a finite tree.
void Generic_recursion::absorb_call( Generic_call call )
{
    const auto same = [&]( const Generic_call& seen )
    { return seen.from == call.from && seen.to == call.to && seen.arguments == call.arguments && seen.at == call.at; };

    if( std::ranges::none_of( generic_calls_, same ) )
    {
        generic_calls_.push_back( std::move( call ) );
    }
}

void Generic_recursion::record_generic_uses( Node_id from, Type_id type, Span at )
{
    record_uses( from, type, at, generic_calls_ );
}

void Generic_recursion::record_uses( Node_id from, Type_id type, Span at, std::vector<Generic_call>& into )
{
    if( !type.is_valid() )
    {
        return;
    }

    const Type& described = table_.get( type );

    switch( described.kind )
    {
    // A pointer is exactly what stops contains_itself seeing this, and D42 asks about instances
    // rather than layout - so the indirection is walked straight through.
    case Type_kind::Pointer:
    case Type_kind::Many_pointer:
    case Type_kind::Run:
    case Type_kind::Array:
        record_uses( from, described.element, at, into );
        break;

    case Type_kind::Struct:
    case Type_kind::Enum:
        if( described.declaration.is_valid() && ast_.is_generic( described.declaration ) )
        {
            const std::vector<Type_id> arguments { described.arguments.begin(), described.arguments.end() };

            into.push_back( Generic_call { .from = from, .to = described.declaration, .arguments = arguments, .at = at } );

            for( const Type_id argument : described.arguments )
            {
                record_uses( from, argument, at, into );
            }
        }
        break;
    case Type_kind::Union:
        for( const Type_id member : described.arguments )
        {
            record_uses( from, member, at, into );
        }
        break;

    // A bare `T` forwards rather than builds, and nothing else can hold a type argument at all.
    default:
        break;
    }
}

// The call itself and every generic type its arguments name, kept only where an argument is a
// written size, so a program without sizes keeps nothing.
void Generic_recursion::record_size_roots( Node_id to, std::span<const Type_id> arguments, Span at )
{
    std::vector<Generic_call> found;

    if( to.is_valid() )
    {
        found.push_back( Generic_call { .from = {}, .to = to, .arguments = { arguments.begin(), arguments.end() }, .at = at } );
    }

    for( const Type_id argument : arguments )
    {
        record_uses( Node_id {}, argument, at, found );
    }

    const auto written = [&]( Type_id argument ) { return table_.is_size( argument ) && table_.get( argument ).width == 0; };

    for( Generic_call& call : found )
    {
        if( std::ranges::any_of( call.arguments, written ) )
        {
            size_roots_.push_back( std::move( call ) );
        }
    }
}

// Deduplicated on insert: `id<i32>( 1 )` written twice is one instantiation, because one is all
// that will be emitted. A linear scan rather than a set - the list is short, and hashing a vector
// of Type_ids to avoid a handful of comparisons is not a trade worth making yet.
std::size_t Generic_recursion::record_instantiation( Node_id declaration, std::vector<Type_id> arguments )
{
    for( std::size_t i = 0; i < instantiations_.size(); ++i )
    {
        const Instantiation& seen = instantiations_[i];

        if( seen.declaration == declaration && seen.arguments == arguments )
        {
            return i;
        }
    }

    instantiations_.push_back( Instantiation { .declaration = declaration, .arguments = std::move( arguments ) } );
    return instantiations_.size() - 1;
}

void Generic_recursion::mark_reported( Node_id declaration )
{
    expanding_.insert( declaration.v );
}

// `f<T>` calling `g<T>` is fine however deep the cycle runs; `f<T>` calling `f<T*>` is not, because
// each step builds a type a level deeper than the last. The shape is decidable rather than capped
// at a depth nobody can justify: a cycle carrying at least one edge that builds a type.
bool Generic_recursion::check()
{
    // The implied edges join the walk only: the instance queue follows calls, not these.
    const std::size_t exported = generic_calls_.size();
    generic_calls_.insert( generic_calls_.end(), implied_.begin(), implied_.end() );

    std::vector<Step> path;

    for( const Generic_call& edge : generic_calls_ )
    {
        if( expanding_.contains( edge.from.v ) )
        {
            continue;
        }

        path.clear();
        path.push_back( Step { .generic = edge.from, .grown_at = std::nullopt } );

        expands_forever( edge.from, path );
    }

    generic_calls_.resize( exported );
    return expanding_.empty();
}

// Depth-first from one generic, each step carrying the growing edge that entered it. Only growth
// inside a cycle counts: `chain<T>` naming `box<chain<T>>` grows on the way in, but box's own
// forwarding cycle is finite. Returns whether it reported, so a cycle is named once rather than
// once per way round it.
bool Generic_recursion::expands_forever( Node_id generic, std::vector<Step>& path )
{
    for( const Generic_call& edge : generic_calls_ )
    {
        // Into a cycle already named, from a generic that only leads to it.
        if( edge.from != generic || expanding_.contains( edge.to.v ) )
        {
            continue;
        }

        // The span of the *growing* call, kept rather than a flag: the edge that closes the cycle
        // is usually an innocent-looking forward, and pointing at it would name the wrong line.
        std::optional<Span> grows;
        bool                by_size = false;

        for( const Type_id argument : edge.arguments )
        {
            if( !grows && wraps_a_parameter( argument ) )
            {
                grows   = edge.at;
                by_size = table_.is_size( argument );
            }
        }

        const auto closing = std::ranges::find_if( path, [&]( const Step& step ) { return step.generic == edge.to; } );

        if( closing == path.end() )
        {
            path.push_back( Step { .generic = edge.to, .grown_at = grows, .by_size = by_size } );

            const bool reported = expands_forever( edge.to, path );

            path.pop_back();

            if( reported )
            {
                return true;
            }

            continue;
        }

        // The edge into `closing` is outside the cycle; every later one is on it.
        std::optional<Span> grown_at;
        bool                grown_by_size = false;

        for( auto step = std::next( closing ); step != path.end() && !grown_at; ++step )
        {
            grown_at      = step->grown_at;
            grown_by_size = step->by_size;
        }

        if( !grown_at )
        {
            grown_at      = grows;
            grown_by_size = by_size;
        }

        if( !grown_at )
        {
            continue; // a cycle that forwards its parameters is finite, and is ordinary recursion
        }

        const auto name_of = [&]( Node_id declaration )
        {
            const std::string_view tilde = ast_.kind( declaration ) == Node_kind::Destructor_decl ? "~" : "";
            return fmt::format( "{}{}", tilde, interner_.text( ast_.name( declaration ) ) );
        };

        // The cycle as written, from where it closes: `f` -> `g` -> `f`. A self-call renders as
        // `f` -> `f`, which reads correctly without a second phrasing for it.
        std::string cycle;

        for( auto step = closing; step != path.end(); ++step )
        {
            cycle += fmt::format( "`{}` to ", name_of( step->generic ) );
            expanding_.insert( step->generic.v );
        }

        cycle += fmt::format( "`{}`", name_of( edge.to ) );

        if( grown_by_size )
        {
            reporter_.error_at(
                *grown_at,
                fmt::format( "`{}` is instantiated with a different size each time round", name_of( edge.to ) ),
                fmt::format(
                    "{}, and each round computes a size from the last - so no set of instances ever "
                    "finishes; forward the size parameter itself",
                    cycle
                )
            );
            return true;
        }

        reporter_.error_at(
            *grown_at,
            fmt::format( "`{}` is instantiated with a bigger type argument each time round", name_of( edge.to ) ),
            fmt::format(
                "{}, and each round builds a type out of the last - so no set of instances ever "
                "finishes; forward the parameter itself, or take the built type as a second parameter",
                cycle
            )
        );

        return true;
    }

    return false;
}

// Is this argument a *construction* over a type parameter rather than one of them? `T` is not, `T*`
// is. One level is all the question needs: one per step around a cycle is already unbounded.
bool Generic_recursion::wraps_a_parameter( Type_id argument ) const
{
    if( !argument.is_valid() )
    {
        return false;
    }

    // A bare parameter forwards a type rather than building one, which is why this cannot simply
    // be mentions_parameter.
    if( table_.is_parameter( argument ) )
    {
        return false;
    }

    return table_.mentions_parameter( argument );
}

std::vector<Instantiation> Generic_recursion::take_instantiations()
{
    return std::move( instantiations_ );
}

std::vector<Generic_call> Generic_recursion::take_generic_calls()
{
    return std::move( generic_calls_ );
}

} // namespace keel::sema

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

// The program's edges only: the prelude's `box<T>` has its own, from its constructor to `box<T>`.
std::size_t program_edges( const Typed& p )
{
    return static_cast<std::size_t>( std::ranges::count_if(
        p.types().generic_calls(), [&]( const Generic_call& call ) { return call.to != p.types().table().prelude().box; }
    ) );
}

// The graph is between generics only. A call in `main` already names an instance, so an edge from
// it would be a step towards something that has already arrived - and the walk reads every edge as
// a step.
TEST_CASE( "generic_recursion_records_an_edge_only_from_inside_a_generic", "[sema][generic]" )
{
    SECTION( "a call from a non-generic function is an instantiation and not an edge" )
    {
        const Typed p( "void g<T>( T a ) where T : copyable { }\ni32 main() { g<i32>( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( program_edges( p ) == 0 );
        REQUIRE( p.types().instantiations().size() == 1 );
    }

    SECTION( "the same call from inside a generic is an edge" )
    {
        const Typed p( "void g<T>( T a ) where T : copyable { }\n"
                       "void f<T>( T a ) where T : copyable { g<T>( a ); }\n"
                       "i32 main() { f<i32>( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( program_edges( p ) == 1 );
    }
}

TEST_CASE( "type_checker_refuses_a_generic_that_expands_forever", "[sema][generic]" )
{
    SECTION( "a generic calling itself at its own parameter is ordinary recursion" )
    {
        const Typed p( "void f<T>( T a ) where T : copyable { f<T>( a ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a cycle that forwards its parameters is finite" )
    {
        // `f<i32>` needs `g<i32>` needs `f<i32>`, which is already emitted. Going round twice
        // produces nothing new, so the set closes.
        const Typed p( "void g<T>( T a ) where T : copyable { }\n"
                       "void f<T>( T a ) where T : copyable { g<T>( a ); }\n"
                       "void h<T>( T a ) where T : copyable { f<T>( a ); g<T>( a ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "building a type out of the parameter is fine when nothing leads back" )
    {
        // One step deeper and then stop: `f<i32>` needs `g<i32*>` and that is the end of it.
        const Typed p( "void g<T>( T a ) where T : copyable { }\n"
                       "void f<T>( T a ) where T : copyable { T* p = &a; g<T*>( p ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    // M7 slice 5. An address seeds an instantiation exactly as a call does, so it has to be an edge
    // of the same graph - otherwise the one construct that can reach a generic without calling it is
    // the one construct that expands forever unchecked.
    SECTION( "taking an instance's address is an edge like a call is" )
    {
        const Typed p( "void f<T>( T a ) where T : copyable { T* p = &a; fn( T* ) -> void q = &f<T*>; }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`f` is instantiated with a bigger type argument each time round" ) != std::string::npos );
        REQUIRE( p.rendered().find( "&f<T*>" ) != std::string::npos );
    }

    SECTION( "a generic calling itself with a pointer to its own parameter is refused" )
    {
        // `f<i32>`, `f<i32*>`, `f<i32**>` - no set of instances finishes.
        const Typed p( "void f<T>( T a ) where T : copyable { T* p = &a; f<T*>( p ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`f` is instantiated with a bigger type argument each time round" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`f` to `f`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "f<T*>( p )" ) != std::string::npos );
    }

    SECTION( "the same through a second generic" )
    {
        // The cycle is what matters, not the self-call: `g` grows it and `f` carries it back.
        const Typed p( "void g<T>( T a ) where T : copyable { T* p = &a; f<T*>( p ); }\n"
                       "void f<T>( T a ) where T : copyable { g<T>( a ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );

        // The cycle is rendered from wherever the walk entered it, so either rotation is a correct
        // description of the same thing. What matters is that both members are named and that the
        // span is the call that *builds* the type rather than the one that forwards it.
        REQUIRE( p.rendered().find( "`g` to `f` to `g`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "f<T*>( p )" ) != std::string::npos );
    }

    SECTION( "a growing call into a forwarding cycle is finite" )
    {
        // `outer<i32>` needs `depth<i32*>`, which needs only itself.
        const Typed p( "i32 depth<U>( U a, i32 n ) where U : copyable { return depth<U>( a, n ); }\n"
                       "i32 outer<T>( T a ) where T : copyable { return depth<T*>( &a, 3 ); }\n"
                       "i32 main() { return outer<i32>( 5 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and a growing cycle after one is still refused, once" )
    {
        const Typed p( "i32 depth<U>( U a, i32 n ) where U : copyable { return depth<U>( a, n ); }\n"
                       "i32 grow<T>( T a ) where T : copyable { T* p = &a; return depth<T*>( p, 1 ) + grow<T*>( p ); }\n"
                       "i32 outer<T>( T a ) where T : copyable { return grow<T>( a ); }\n"
                       "i32 main() { return outer<i32>( 5 ); }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`grow` to `grow`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "grow<T*>( p )" ) != std::string::npos );
    }

    SECTION( "it is reported without anything being instantiated" )
    {
        // The graph is over declarations, so `main` naming none of them changes nothing. Reporting
        // only once something asked for an instance would be the deferred check this avoids.
        const Typed p( "void f<T>( T a ) where T : copyable { T* p = &a; f<T*>( p ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "bigger type argument" ) != std::string::npos );
    }
}

// D78: a size forwards like a `T`, and a suffix over it is growth in either direction.
TEST_CASE( "generic_recursion_over_a_size_parameter", "[sema][generic][size_params]" )
{
    SECTION( "a cycle forwarding `N` is finite" )
    {
        const Typed p( "u64 f<u64 N>( i32[N] a, u64 d ) { if( d == 0 ) { return N; } return f<N>( a, d - 1 ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "growing `T` beside a forwarded `N` is still refused" )
    {
        const Typed p( "void f<T, u64 N>( T[N] a ) where T : copyable { T* p = &a[0]; f<T*, N>( [ p, .. ] ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`f` is instantiated with a bigger type argument each time round" ) != std::string::npos );
    }

    SECTION( "a size computed from `N` is refused, smaller as well as bigger" )
    {
        for( const std::string_view op : { "+", "-" } )
        {
            const Typed p(
                fmt::format( "void f<u64 N>( i32[N] a ) {{ i32[N {} 1] b = [ 0, .. ]; f( b ); }}\n", op ) +
                "i32 main() { return 0; }"
            );

            INFO( p.rendered() );
            REQUIRE( p.errors() == 1 );
            REQUIRE( p.rendered().find( "`f` is instantiated with a different size each time round" ) != std::string::npos );
            REQUIRE( p.rendered().find( "forward the size parameter itself" ) != std::string::npos );
        }
    }
}

// The other half of the same question. Size is about by-value members, so a pointer hides the cycle
// from contains_itself entirely - and `Bad<T>` holding a `Bad<Box<T>>*` still names an instance set
// that never finishes. D42 is what refuses it, over the field edges record_generic_uses records.
TEST_CASE( "type_checker_refuses_a_generic_aggregate_that_grows_behind_a_pointer", "[sema][generic][aggregate]" )
{
    constexpr std::string_view box = "struct Box<T> where T : copyable { T v; };\n";

    SECTION( "a field growing its own parameter is refused" )
    {
        // Accepted until the field edges existed, and then emitted forever: the struct order
        // interns `Bad<Box<i32>>` to walk it, which interns `Bad<Box<Box<i32>>>`, without end.
        const Typed p(
            std::string( box ) + "class Bad<T> where T : copyable { Bad<Box<T>>* next; };\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE(
            p.rendered().find( "`Bad` is instantiated with a bigger type argument each time round" ) != std::string::npos
        );
    }

    SECTION( "the cycle may run through a second aggregate" )
    {
        // Reported at the growing field rather than at the one that closes the cycle - `B` holding
        // an `A<U>*` is the innocent-looking half, and naming it would point at the wrong line.
        const Typed p(
            std::string( box ) + "class A<T> where T : copyable { B<Box<T>>* b; };\n"
                                 "class B<U> where U : copyable { A<U>* a; };\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "bigger type argument each time round" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`A` to `B` to `A`" ) != std::string::npos );
    }

    SECTION( "the cycle named is the one that grows, not the prelude box's own" )
    {
        const Typed p( "struct S<T> where T : copyable { T t; box<S<fn( T ) -> void>>* s; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`S` is instantiated with a bigger type argument each time round" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`S` to `S`" ) != std::string::npos );
    }

    SECTION( "the cycle may close through a type argument rather than the type itself" )
    {
        // `Box<A<Box<T>>>` names `Box` at the top and `A` one level in. Only the inner mention
        // closes a cycle, so the arguments have to be walked and not just the type they belong to.
        const Typed p(
            std::string( box ) + "class A<T> where T : copyable { Box<A<Box<T>>>* b; };\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`A` is instantiated with a bigger type argument each time round" ) != std::string::npos );
    }

    SECTION( "forwarding the parameter round the cycle is finite" )
    {
        // The rule is about growth, not about recursion: these two need `A<i32>` and `B<i32>` and
        // nothing further, however long the cycle runs.
        const Typed p( "class A<T> where T : copyable { B<T>* b; };\n"
                       "class B<U> where U : copyable { A<U>* a; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "building one level without a cycle is fine" )
    {
        // `Vector<T>` storing a `T*` is the first thing a real container writes, and `Box<T*>`
        // needs exactly two instances rather than unboundedly many.
        const Typed p(
            std::string( box ) + "class Vector<T> where T : copyable { T* data; };\n"
                                 "class Wrapper<T> where T : copyable { Box<T*>* b; };\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a plain aggregate may hold an instance of a generic one" )
    {
        // The field pass skips a non-generic aggregate, because `Box<i32>` there is already an
        // instance rather than a step towards one. Nothing observable turns on it - a declaration
        // with no parameters cannot write an argument that mentions one - so the guard keeps the
        // graph's invariant true by construction, and this section only pins the accepting half.
        const Typed p(
            std::string( box ) + "struct Holder { Box<i32>* b; };\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

// A generic aggregate in a generic *signature* is where substitution over a struct type happens:
// `Box<T>` is written once and every instantiation reads a different one. Reachable at the checker
// even though nothing emits one yet, because a call site substitutes the parameter's type to check
// its argument against it.
TEST_CASE( "type_checker_substitutes_a_generic_aggregate", "[sema][generic][aggregate]" )
{
    const std::string_view box = "struct Box<T> where T : copyable { T v; };\n";

    SECTION( "a parameter of type `Box<T>` names the instantiation at the call" )
    {
        // The diagnostic has to say `Box<i32>`, which means substitute rebuilt the struct type from
        // its arguments rather than handing back the template.
        const Typed p(
            std::string( box ) + "void take<T>( Box<T> b ) where T : copyable { }\n"
                                 "i32 main() { i32 x = 1; take<i32>( x ); return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "expected `Box<i32>`, but got `i32`" ) != std::string::npos );

        // Not `Box<T><i32>`, which is what composing the new name out of the old rendering gives.
        REQUIRE( p.rendered().find( "Box<T>" ) == std::string::npos );
    }

    SECTION( "a field of one is read at the substituted type" )
    {
        const Typed p(
            std::string( box ) + "T peek<T>( Box<T> b ) where T : copyable { return b.v; }\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and the wrong field type is named against the parameter" )
    {
        const Typed p(
            std::string( box ) + "bool peek<T>( Box<T> b ) where T : copyable { return b.v; }\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "expected `bool`, but got `T`" ) != std::string::npos );
    }

    SECTION( "D42 counts a generic aggregate as building a type, exactly as a pointer does" )
    {
        // `f<i32>` would need `f<Box<i32>>` would need `f<Box<Box<i32>>>`. The rule was written for
        // `T*` and needed nothing added for this - wraps_a_parameter asks whether the argument
        // *mentions* a parameter without *being* one, which both shapes answer the same way.
        const Typed p(
            std::string( box ) + "void f<T>( T a ) where T : copyable { Box<T> b; f<Box<T>>( b ); }\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "is instantiated with a bigger type argument each time round" ) != std::string::npos );
    }

    SECTION( "forwarding the parameter itself is still finite" )
    {
        const Typed p(
            std::string( box ) + "void g<T>( Box<T> b ) where T : copyable { }\n"
                                 "void f<T>( Box<T> b ) where T : copyable { g<T>( b ); }\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

} // namespace keel
#endif
