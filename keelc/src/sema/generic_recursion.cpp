#include "sema/generic_recursion.h"
#include <fmt/format.h>
#include <algorithm>
#include <optional>

namespace keel
{
namespace sema
{

// An edge exists only between generics: a call in `main` is already an instance rather than a step
// towards one, so the caller is the guard rather than the call site.
void Generic_recursion::record_call( Node_id from, Node_id to, std::vector<Type_id> arguments, Span at )
{
    if( is_generic( ast_, from ) )
    {
        generic_calls_.push_back( Generic_call { .from = from, .to = to, .arguments = std::move( arguments ), .at = at } );
    }
}

// A field naming another generic is the same edge a call is, so it goes in the same graph: a cycle
// through an aggregate and a function is then found the way one through two functions is.
//
// `Bad<Box<T>>` is two facts, so the arguments are walked as well as the type. No visited-set - an
// interned type is a finite tree.
void Generic_recursion::record_generic_uses( Node_id from, Type_id type, Span at )
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
        record_generic_uses( from, described.element, at );
        break;

    case Type_kind::Struct:
    case Type_kind::Enum:
        if( described.declaration.is_valid() && is_generic( ast_, described.declaration ) )
        {
            const std::vector<Type_id> arguments { described.arguments.begin(), described.arguments.end() };

            generic_calls_.push_back(
                Generic_call { .from = from, .to = described.declaration, .arguments = arguments, .at = at }
            );

            for( const Type_id argument : described.arguments )
            {
                record_generic_uses( from, argument, at );
            }
        }
        break;

    // A bare `T` forwards rather than builds, and nothing else can hold a type argument at all.
    default:
        break;
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
void Generic_recursion::check()
{
    std::vector<Node_id> path;

    for( const Generic_call& edge : generic_calls_ )
    {
        if( expanding_.contains( edge.from.v ) )
        {
            continue;
        }

        path.clear();
        path.push_back( edge.from );

        expands_forever( edge.from, path, std::nullopt );
    }
}

// Depth-first from one generic, carrying whether any edge on the current path built a type. Returns
// whether it reported, so a cycle is named once rather than once per way round it.
bool Generic_recursion::expands_forever( Node_id generic, std::vector<Node_id>& path, std::optional<Span> grown_at )
{
    for( const Generic_call& edge : generic_calls_ )
    {
        if( edge.from != generic )
        {
            continue;
        }

        // The span of the *growing* call, kept rather than a flag: the edge that closes the cycle
        // is usually an innocent-looking forward, and pointing at it would name the wrong line.
        std::optional<Span> grows = grown_at;

        for( const Type_id argument : edge.arguments )
        {
            if( !grows && wraps_a_parameter( argument ) )
            {
                grows = edge.at;
            }
        }

        const bool closes = std::find( path.begin(), path.end(), edge.to ) != path.end();

        if( closes && grows )
        {
            const auto name_of = [&]( Node_id declaration )
            { return std::string( interner_.text( Symbol_id { ast_.aux( declaration ) } ) ); };

            // The cycle as written, from where it closes: `f` -> `g` -> `f`. A self-call renders as
            // `f` -> `f`, which reads correctly without a second phrasing for it.
            std::string cycle;

            for( auto step = std::find( path.begin(), path.end(), edge.to ); step != path.end(); ++step )
            {
                cycle += fmt::format( "`{}` to ", name_of( *step ) );
            }

            cycle += fmt::format( "`{}`", name_of( edge.to ) );

            for( const Node_id step : path )
            {
                expanding_.insert( step.v );
            }

            reporter_.error_at(
                *grows,
                fmt::format( "`{}` is instantiated with a bigger type argument each time round", name_of( edge.to ) ),
                fmt::format(
                    "{}, and each round builds a type out of the last - so no set of instances ever "
                    "finishes; forward the parameter itself, or take the built type as a second parameter",
                    cycle
                )
            );

            return true;
        }

        if( closes )
        {
            continue; // a cycle that forwards its parameters is finite, and is ordinary recursion
        }

        path.push_back( edge.to );

        const bool reported = expands_forever( edge.to, path, grows );

        path.pop_back();

        if( reported )
        {
            return true;
        }
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

} // namespace sema
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include "sema/checker_test_support.h"

namespace keel
{

// The graph is between generics only. A call in `main` already names an instance, so an edge from
// it would be a step towards something that has already arrived - and the walk reads every edge as
// a step.
TEST_CASE( "generic_recursion_records_an_edge_only_from_inside_a_generic", "[sema][generic]" )
{
    SECTION( "a call from a non-generic function is an instantiation and not an edge" )
    {
        const Typed p( "void g<T>( T a ) where T : Copyable { }\ni32 main() { g<i32>( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.types().generic_calls().empty() );
        REQUIRE( p.types().instantiations().size() == 1 );
    }

    SECTION( "the same call from inside a generic is an edge" )
    {
        const Typed p( "void g<T>( T a ) where T : Copyable { }\n"
                       "void f<T>( T a ) where T : Copyable { g<T>( a ); }\n"
                       "i32 main() { f<i32>( 1 ); return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
        REQUIRE( p.types().generic_calls().size() == 1 );
    }
}

TEST_CASE( "type_checker_refuses_a_generic_that_expands_forever", "[sema][generic]" )
{
    SECTION( "a generic calling itself at its own parameter is ordinary recursion" )
    {
        const Typed p( "void f<T>( T a ) where T : Copyable { f<T>( a ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a cycle that forwards its parameters is finite" )
    {
        // `f<i32>` needs `g<i32>` needs `f<i32>`, which is already emitted. Going round twice
        // produces nothing new, so the set closes.
        const Typed p( "void g<T>( T a ) where T : Copyable { }\n"
                       "void f<T>( T a ) where T : Copyable { g<T>( a ); }\n"
                       "void h<T>( T a ) where T : Copyable { f<T>( a ); g<T>( a ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "building a type out of the parameter is fine when nothing leads back" )
    {
        // One step deeper and then stop: `f<i32>` needs `g<i32*>` and that is the end of it.
        const Typed p( "void g<T>( T a ) where T : Copyable { }\n"
                       "void f<T>( T a ) where T : Copyable { T* p = &a; g<T*>( p ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "a generic calling itself with a pointer to its own parameter is refused" )
    {
        // `f<i32>`, `f<i32*>`, `f<i32**>` - no set of instances finishes.
        const Typed p( "void f<T>( T a ) where T : Copyable { T* p = &a; f<T*>( p ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );
        REQUIRE( p.rendered().find( "`f` is instantiated with a bigger type argument each time round" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`f` to `f`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "f<T*>( p )" ) != std::string::npos );
    }

    SECTION( "the same through a second generic" )
    {
        // The cycle is what matters, not the self-call: `g` grows it and `f` carries it back.
        const Typed p( "void g<T>( T a ) where T : Copyable { T* p = &a; f<T*>( p ); }\n"
                       "void f<T>( T a ) where T : Copyable { g<T>( a ); }\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.errors() == 1 );

        // The cycle is rendered from wherever the walk entered it, so either rotation is a correct
        // description of the same thing. What matters is that both members are named and that the
        // span is the call that *builds* the type rather than the one that forwards it.
        REQUIRE( p.rendered().find( "`g` to `f` to `g`" ) != std::string::npos );
        REQUIRE( p.rendered().find( "f<T*>( p )" ) != std::string::npos );
    }

    SECTION( "it is reported without anything being instantiated" )
    {
        // The graph is over declarations, so `main` naming none of them changes nothing. Reporting
        // only once something asked for an instance would be the deferred check this avoids.
        const Typed p( "void f<T>( T a ) where T : Copyable { T* p = &a; f<T*>( p ); }\ni32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "bigger type argument" ) != std::string::npos );
    }
}

// The other half of the same question. Size is about by-value members, so a pointer hides the cycle
// from contains_itself entirely - and `Bad<T>` holding a `Bad<Box<T>>*` still names an instance set
// that never finishes. D42 is what refuses it, over the field edges record_generic_uses records.
TEST_CASE( "type_checker_refuses_a_generic_aggregate_that_grows_behind_a_pointer", "[sema][generic][aggregate]" )
{
    constexpr std::string_view box = "struct Box<T> where T : Copyable { T v; };\n";

    SECTION( "a field growing its own parameter is refused" )
    {
        // Accepted until the field edges existed, and then emitted forever: the struct order
        // interns `Bad<Box<i32>>` to walk it, which interns `Bad<Box<Box<i32>>>`, without end.
        const Typed p(
            std::string( box ) + "class Bad<T> where T : Copyable { Bad<Box<T>>* next; };\n"
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
            std::string( box ) + "class A<T> where T : Copyable { B<Box<T>>* b; };\n"
                                 "class B<U> where U : Copyable { A<U>* a; };\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "bigger type argument each time round" ) != std::string::npos );
        REQUIRE( p.rendered().find( "`A` to `B` to `A`" ) != std::string::npos );
    }

    SECTION( "the cycle may close through a type argument rather than the type itself" )
    {
        // `Box<A<Box<T>>>` names `Box` at the top and `A` one level in. Only the inner mention
        // closes a cycle, so the arguments have to be walked and not just the type they belong to.
        const Typed p(
            std::string( box ) + "class A<T> where T : Copyable { Box<A<Box<T>>>* b; };\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "`A` is instantiated with a bigger type argument each time round" ) != std::string::npos );
    }

    SECTION( "forwarding the parameter round the cycle is finite" )
    {
        // The rule is about growth, not about recursion: these two need `A<i32>` and `B<i32>` and
        // nothing further, however long the cycle runs.
        const Typed p( "class A<T> where T : Copyable { B<T>* b; };\n"
                       "class B<U> where U : Copyable { A<U>* a; };\n"
                       "i32 main() { return 0; }" );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "building one level without a cycle is fine" )
    {
        // `Vector<T>` storing a `T*` is the first thing a real container writes, and `Box<T*>`
        // needs exactly two instances rather than unboundedly many.
        const Typed p(
            std::string( box ) + "class Vector<T> where T : Copyable { T* data; };\n"
                                 "class Wrapper<T> where T : Copyable { Box<T*>* b; };\n"
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
    const std::string_view box = "struct Box<T> where T : Copyable { T v; };\n";

    SECTION( "a parameter of type `Box<T>` names the instantiation at the call" )
    {
        // The diagnostic has to say `Box<i32>`, which means substitute rebuilt the struct type from
        // its arguments rather than handing back the template.
        const Typed p(
            std::string( box ) + "void take<T>( Box<T> b ) where T : Copyable { }\n"
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
            std::string( box ) + "T peek<T>( Box<T> b ) where T : Copyable { return b.v; }\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }

    SECTION( "and the wrong field type is named against the parameter" )
    {
        const Typed p(
            std::string( box ) + "bool peek<T>( Box<T> b ) where T : Copyable { return b.v; }\n"
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
            std::string( box ) + "void f<T>( T a ) where T : Copyable { Box<T> b; f<Box<T>>( b ); }\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.rendered().find( "is instantiated with a bigger type argument each time round" ) != std::string::npos );
    }

    SECTION( "forwarding the parameter itself is still finite" )
    {
        const Typed p(
            std::string( box ) + "void g<T>( Box<T> b ) where T : Copyable { }\n"
                                 "void f<T>( Box<T> b ) where T : Copyable { g<T>( b ); }\n"
                                 "i32 main() { return 0; }"
        );

        INFO( p.rendered() );
        REQUIRE( p.clean() );
    }
}

} // namespace keel
#endif
