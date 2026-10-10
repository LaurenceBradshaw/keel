// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/instances.h"
#include <algorithm>

namespace keel
{

namespace
{

bool already_queued( const std::vector<Instantiation>& pending, const Instantiation& candidate )
{
    return std::any_of(
        pending.begin(),
        pending.end(),
        [&candidate]( const Instantiation& seen )
        { return seen.declaration == candidate.declaration && seen.arguments == candidate.arguments; }
    );
}

// An argument mentioning a type parameter - `T`, or `T*` - means the call was written inside a
// generic and names a template rather than an instance. Those are the edges, not the seeds.
bool is_closed( const Types& types, const Instantiation& candidate )
{
    return std::none_of(
        candidate.arguments.begin(),
        candidate.arguments.end(),
        [&types]( Type_id argument ) { return types.table().mentions_parameter( argument ); }
    );
}

void queue( std::vector<Instantiation>& pending, const Types& types, Instantiation candidate )
{
    if( is_closed( types, candidate ) && !already_queued( pending, candidate ) )
    {
        pending.push_back( std::move( candidate ) );
    }
}

// The destructor of each closed instance of a generic aggregate. No call site names one, so
// nothing else would seed it.
void queue_destructors( const Ast& ast, const Types& types, std::vector<Instantiation>& pending )
{
    for( const Type_id composite_type_id : types.table().composite_types() )
    {
        const Type& composite_type = types.table().get( composite_type_id );

        // An enum is skipped rather than walked: its destructor is synthesised by lower(), not
        // declared - and ast.members asserts on one.
        if( !composite_type.declaration.is_valid() || !ast.is_generic( composite_type.declaration ) ||
            ast.kind( composite_type.declaration ) == Node_kind::Enum_decl )
        {
            continue;
        }

        for( const Node_id decl : ast.members( composite_type.declaration ) )
        {
            if( ast.kind( decl ) == Node_kind::Destructor_decl )
            {
                queue(
                    pending,
                    types,
                    { .declaration = decl, .arguments = { composite_type.arguments.begin(), composite_type.arguments.end() } }
                );
                break;
            }
        }
    }
}

} // namespace

// The checker's list is only the seed. A call site inside a generic writes `g<T>`, which names no
// instance at all until `T` is known - so the real set is the closure of that list under the
// generic call graph, and reaching it needs a worklist rather than a pass.
//
// It terminates because Generic_recursion::check refused the shape that would not: a cycle whose
// arguments grow a level each time round. Every other cycle forwards its parameters unchanged, so
// going round it twice produces an instantiation that is already in the set.
std::vector<Instantiation> instances_to_emit( const Ast& ast, Types& types )
{
    std::vector<Instantiation> pending;

    for( const Instantiation& seed : types.instantiations() )
    {
        queue( pending, types, seed );
    }

    queue_destructors( ast, types, pending );

    // Indexed rather than iterated: the loop appends to what it is walking.
    for( std::size_t at = 0; at < pending.size(); ++at )
    {
        const Instantiation instance = pending[at];
        const Bindings      bindings = bindings_for( ast, types, instance );

        // What this instance's body reaches. `g<T>` under `{ T -> i32 }` is `g<i32>`, which no call
        // site ever wrote and which nothing else would ever emit.
        for( const Generic_call& edge : types.generic_calls() )
        {
            if( edge.from != instance.declaration )
            {
                continue;
            }

            Instantiation reached { .declaration = edge.to, .arguments = {} };

            for( const Type_id argument : edge.arguments )
            {
                reached.arguments.push_back( types.table().substitute( argument, bindings ) );
            }

            queue( pending, types, std::move( reached ) );
        }
    }

    return pending;
}

// Positional, which is what D39's spelling guarantees: the nth type argument binds the nth parameter.
Bindings bindings_for( const Ast& ast, const Types& types, const Instantiation& instance )
{
    const std::vector<Node_id> parameters = ast.type_parameters( ast.type_param_list( instance.declaration ) );

    Bindings bindings;

    for( std::size_t i = 0; i < parameters.size() && i < instance.arguments.size(); ++i )
    {
        bindings.emplace( types.type_of( parameters[i] ).v, instance.arguments[i] );
    }

    return bindings;
}

} // namespace keel
#ifdef ENABLE_UNIT_TESTS
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "parse/loader.h"
#include "sema/resolver.h"

namespace keel
{
namespace
{

// The front end, then the instance list each case asks about, each spelled `name<args>`.
struct Reached
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;
    Ast            ast;
    Resolution     resolution;
    Types          types;

    std::vector<std::string> instances;

    explicit Reached( std::string_view source )
    {
        const File_id file = sm.add_file( "t.kl", std::string( source ) );

        Program program = load_program( file, sm, interner, literals, diags, {}, {} );
        ast             = std::move( program.ast );
        resolution      = resolve( ast, sm, interner, diags, program.imports );
        types           = type_check( ast, resolution, literals, sm, interner, diags );

        if( diags.has_errors() )
        {
            return;
        }

        for( const Instantiation& instance : instances_to_emit( ast, types ) )
        {
            std::string spelled = ast.kind( instance.declaration ) == Node_kind::Destructor_decl ? "~" : "";
            spelled += interner.text( ast.name( instance.declaration ) );

            for( std::size_t i = 0; i < instance.arguments.size(); ++i )
            {
                spelled += fmt::format( "{}{}", i == 0 ? "<" : ", ", types.table().name( instance.arguments[i] ) );
            }

            instances.push_back( spelled + ">" );
        }
    }

    bool clean() const
    {
        return !diags.has_errors();
    }
};

} // namespace

TEST_CASE( "instances_to_emit_reaches_every_instance_once", "[ir][instances][generic]" )
{
    SECTION( "one per set of type arguments a call site wrote" )
    {
        Reached r( "T id<T>( T a ) where T : Copyable { return a; }\n"
                   "i32 main() { i32 a = id<i32>( 1 ); bool b = id<bool>( true ); return a; }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "id<i32>", "id<bool>" } );
    }

    SECTION( "the same arguments twice are one instance" )
    {
        Reached r( "T id<T>( T a ) where T : Copyable { return a; }\ni32 main() { return id<i32>( 3 ) + id<i32>( 4 ); }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "id<i32>" } );
    }

    SECTION( "a call inside a generic reaches its instance through the caller's" )
    {
        // `inner<T>` names no instance itself; `outer<i32>` is what makes it `inner<i32>`.
        Reached r( "T inner<T>( T a ) where T : Copyable { return a; }\n"
                   "T outer<T>( T a ) where T : Copyable { return inner<T>( a ); }\n"
                   "i32 main() { return outer<i32>( 1 ); }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "outer<i32>", "inner<i32>" } );
    }

    SECTION( "the substitution reaches inside a type constructor" )
    {
        Reached r( "T* inner<T>( T* a ) { return a; }\n"
                   "T* outer<T>( T* a ) { return inner<T>( a ); }\n"
                   "i32 main() { i32 v = 0; i32* p = outer<i32>( &v ); return 0; }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "outer<i32>", "inner<i32>" } );
    }

    SECTION( "a generic nothing calls has no instance" )
    {
        Reached r( "T id<T>( T a ) where T : Copyable { return a; }\ni32 main() { return 0; }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances.empty() );
    }
}

TEST_CASE( "instances_to_emit_seeds_a_generic_aggregate's_destructor", "[ir][instances][generic][aggregate]" )
{
    SECTION( "one per closed instance of the aggregate" )
    {
        Reached r( "class Box<T> { public u64 n; Box( u64 m ) { n = m; } ~Box() { n = 0; } };\n"
                   "i32 main() { Box<u64> a = Box<u64>( 7 ); Box<i32> b = Box<i32>( 8 ); return 0; }" );

        REQUIRE( r.clean() );
        REQUIRE( std::count( r.instances.begin(), r.instances.end(), "~Box<u64>" ) == 1 );
        REQUIRE( std::count( r.instances.begin(), r.instances.end(), "~Box<i32>" ) == 1 );
    }

    SECTION( "never for the open form" )
    {
        Reached r( "class Box<T> { public u64 n; Box( u64 m ) { n = m; } ~Box() { n = 0; } };\ni32 main() { return 0; }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances.empty() );
    }
}

// A generic method's instance carries the receiver's arguments and then its own, which is the
// order of the parameters in its slot.
TEST_CASE( "instances_to_emit_gives_a_generic_method_one_instance_per_call", "[ir][instances][generic]" )
{
    SECTION( "two of the method's own arguments on one receiver are two instances" )
    {
        Reached r(
            "struct Box<T> where T : Copyable\n"
            "{\n"
            "    T v;\n"
            "    U as<U>( U u ) const where U : Copyable { return u; }\n"
            "};\n"
            "i32 main() { Box<i32> b = Box { 1 }; i64 x = b.as( cast<i64>( 2 ) ); bool y = b.as<bool>( true ); return 0; }"
        );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "as<i32, i64>", "as<i32, bool>" } );
    }

    SECTION( "on a type that is not generic, only its own" )
    {
        Reached r( "struct Plain { i32 n; U as<U>( U u ) const where U : Copyable { return u; } };\n"
                   "i32 main() { Plain p = Plain { 1 }; return p.as<i32>( 3 ); }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "as<i32>" } );
    }

    SECTION( "one generic method calling another reaches it through the caller's instance" )
    {
        Reached r( "struct Box<T> where T : Copyable\n"
                   "{\n"
                   "    T v;\n"
                   "    U inner<U>( U u ) const where U : Copyable { return u; }\n"
                   "    U outer<U>( U u ) const where U : Copyable { return inner<U>( u ); }\n"
                   "};\n"
                   "i32 main() { Box<bool> b = Box { true }; return b.outer<i32>( 3 ); }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "outer<bool, i32>", "inner<bool, i32>" } );
    }
}

TEST_CASE( "bindings_for_binds_parameters_in_order", "[ir][instances][generic]" )
{
    Reached r( "A first<A, B>( A a, B b ) where A : Copyable { return a; }\n"
               "i32 main() { return first<i32, bool>( 1, true ); }" );

    REQUIRE( r.clean() );

    const std::vector<Instantiation> instances = instances_to_emit( r.ast, r.types );
    REQUIRE( instances.size() == 1 );

    const Bindings             bindings   = bindings_for( r.ast, r.types, instances[0] );
    const std::vector<Node_id> parameters = r.ast.type_parameters( r.ast.type_param_list( instances[0].declaration ) );

    REQUIRE( bindings.size() == 2 );
    REQUIRE( bindings.at( r.types.type_of( parameters[0] ).v ) == r.types.table().integer( 32, true ) );
    REQUIRE( bindings.at( r.types.type_of( parameters[1] ).v ) == r.types.table().builtin( Type_kind::Bool ) );
}

} // namespace keel
#endif
