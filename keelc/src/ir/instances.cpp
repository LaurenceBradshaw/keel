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

// A closed instance of a generic aggregate - the only composites whose members need instances.
// An enum is skipped: its destructor is synthesised by lower(), not declared, and ast.members
// asserts on one.
bool is_generic_aggregate( const Ast& ast, const Type& composite )
{
    return composite.declaration.is_valid() && ast.is_generic( composite.declaration ) &&
           ast.kind( composite.declaration ) != Node_kind::Enum_decl;
}

// The destructor of one closed instance of a generic aggregate. No call site names one, so
// nothing else would seed it.
void queue_destructor( const Ast& ast, const Types& types, Type_id composite, std::vector<Instantiation>& pending )
{
    const Type& composite_type = types.table().get( composite );

    if( !is_generic_aggregate( ast, composite_type ) )
    {
        return;
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

// The methods answering an interface, for one closed instance of a generic implementer: a vtable or
// a call through a bound reaches them, and neither is a call site the checker recorded.
void queue_conformances( const Ast& ast, const Types& types, Type_id composite, std::vector<Instantiation>& pending )
{
    const Type& composite_type = types.table().get( composite );

    if( !is_generic_aggregate( ast, composite_type ) )
    {
        return;
    }

    for( const Conformance& conformance : types.conformances().conformances )
    {
        if( conformance.implementer != composite_type.declaration )
        {
            continue;
        }

        for( const Node_id method : conformance.methods )
        {
            if( method.is_valid() )
            {
                queue(
                    pending,
                    types,
                    { .declaration = method, .arguments = { composite_type.arguments.begin(), composite_type.arguments.end() } }
                );
            }
        }
    }
}

} // namespace

Instance_queue::Instance_queue( const Ast& ast, Types& types )
    : ast_( ast ),
      types_( types )
{
    for( const Instantiation& seed : types_.instantiations() )
    {
        queue( pending_, types_, seed );
    }

    queue_new_composites();
}

std::optional<Instantiation> Instance_queue::next()
{
    queue_new_composites();

    if( at_ == pending_.size() )
    {
        return std::nullopt;
    }

    // A copy: queueing what it reaches appends to pending_.
    const Instantiation instance = pending_[at_++];
    const Bindings      bindings = bindings_for( ast_, types_, instance );

    // What this instance's body reaches. `g<T>` under `{ T -> i32 }` is `g<i32>`, which no call
    // site ever wrote and which nothing else would ever emit.
    for( const Generic_call& edge : types_.generic_calls() )
    {
        if( edge.from != instance.declaration )
        {
            continue;
        }

        Instantiation reached { .declaration = edge.to, .arguments = {} };

        for( const Type_id argument : edge.arguments )
        {
            reached.arguments.push_back( types_.table().substitute( argument, bindings ) );
        }

        queue( pending_, types_, std::move( reached ) );
    }

    return instance;
}

// From where the last call stopped: composite_types() is in interning order, so whatever was
// interned since is at the end.
void Instance_queue::queue_new_composites()
{
    const std::vector<Type_id> composites = types_.table().composite_types();

    for( ; composites_seen_ < composites.size(); ++composites_seen_ )
    {
        queue_destructor( ast_, types_, composites[composites_seen_], pending_ );
        queue_conformances( ast_, types_, composites[composites_seen_], pending_ );
    }
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

        Instance_queue queue( ast, types );

        while( const std::optional<Instantiation> next = queue.next() )
        {
            const Instantiation& instance = *next;

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

TEST_CASE( "instance_queue_reaches_every_instance_once", "[ir][instances][generic]" )
{
    SECTION( "one per set of type arguments a call site wrote" )
    {
        Reached r( "T id<T>( T a ) where T : copyable { return a; }\n"
                   "i32 main() { i32 a = id<i32>( 1 ); bool b = id<bool>( true ); return a; }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "id<i32>", "id<bool>" } );
    }

    SECTION( "the same arguments twice are one instance" )
    {
        Reached r( "T id<T>( T a ) where T : copyable { return a; }\ni32 main() { return id<i32>( 3 ) + id<i32>( 4 ); }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "id<i32>" } );
    }

    SECTION( "a call inside a generic reaches its instance through the caller's" )
    {
        // `inner<T>` names no instance itself; `outer<i32>` is what makes it `inner<i32>`.
        Reached r( "T inner<T>( T a ) where T : copyable { return a; }\n"
                   "T outer<T>( T a ) where T : copyable { return inner<T>( a ); }\n"
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
        Reached r( "T id<T>( T a ) where T : copyable { return a; }\ni32 main() { return 0; }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances.empty() );
    }
}

TEST_CASE( "instance_queue_seeds_a_generic_aggregate's_destructor", "[ir][instances][generic][aggregate]" )
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
TEST_CASE( "instance_queue_gives_a_generic_method_one_instance_per_call", "[ir][instances][generic]" )
{
    SECTION( "two of the method's own arguments on one receiver are two instances" )
    {
        Reached r(
            "struct Box<T> where T : copyable\n"
            "{\n"
            "    T v;\n"
            "    U as<U>( U u ) const where U : copyable { return u; }\n"
            "};\n"
            "i32 main() { Box<i32> b = Box { 1 }; i64 x = b.as( cast<i64>( 2 ) ); bool y = b.as<bool>( true ); return 0; }"
        );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "as<i32, i64>", "as<i32, bool>" } );
    }

    SECTION( "on a type that is not generic, only its own" )
    {
        Reached r( "struct Plain { i32 n; U as<U>( U u ) const where U : copyable { return u; } };\n"
                   "i32 main() { Plain p = Plain { 1 }; return p.as<i32>( 3 ); }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "as<i32>" } );
    }

    SECTION( "one generic method calling another reaches it through the caller's instance" )
    {
        Reached r( "struct Box<T> where T : copyable\n"
                   "{\n"
                   "    T v;\n"
                   "    U inner<U>( U u ) const where U : copyable { return u; }\n"
                   "    U outer<U>( U u ) const where U : copyable { return inner<U>( u ); }\n"
                   "};\n"
                   "i32 main() { Box<bool> b = Box { true }; return b.outer<i32>( 3 ); }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances == std::vector<std::string> { "outer<bool, i32>", "inner<bool, i32>" } );
    }
}

TEST_CASE( "bindings_for_binds_parameters_in_order", "[ir][instances][generic]" )
{
    Reached r( "A first<A, B>( A a, B b ) where A : copyable { return a; }\n"
               "i32 main() { return first<i32, bool>( 1, true ); }" );

    REQUIRE( r.clean() );

    Instance_queue                     queue( r.ast, r.types );
    const std::optional<Instantiation> instance = queue.next();
    REQUIRE( instance.has_value() );
    REQUIRE_FALSE( queue.next().has_value() );

    const Bindings             bindings   = bindings_for( r.ast, r.types, *instance );
    const std::vector<Node_id> parameters = r.ast.type_parameters( r.ast.type_param_list( instance->declaration ) );

    REQUIRE( bindings.size() == 2 );
    REQUIRE( bindings.at( r.types.type_of( parameters[0] ).v ) == r.types.table().integer( 32, true ) );
    REQUIRE( bindings.at( r.types.type_of( parameters[1] ).v ) == r.types.table().builtin( Type_kind::Bool ) );
}

// A vtable slot reaches a generic implementer's method without a call site the checker recorded.
TEST_CASE( "instance_queue_reaches_a_generic_implementer's_methods", "[ir][instances][generic][interfaces]" )
{
    const std::string shape = "interface Shape { f64 area() const; };\n"
                              "struct Pair<T> implements Shape where T : copyable { T a; f64 area() const { return 2.0; } };\n"
                              "f64 measure( const ref Shape s ) { return s.area(); }\n";

    SECTION( "one per closed instance" )
    {
        Reached r(
            shape + "i32 main() { Pair<i32> p = Pair { 1 }; Pair<bool> q = Pair { true }; "
                    "f64 x = measure( p ) + measure( q ); return 0; }"
        );

        REQUIRE( r.clean() );
        REQUIRE( std::count( r.instances.begin(), r.instances.end(), "area<i32>" ) == 1 );
        REQUIRE( std::count( r.instances.begin(), r.instances.end(), "area<bool>" ) == 1 );
    }

    SECTION( "never for the open form" )
    {
        Reached r( shape + "i32 main() { return 0; }" );

        REQUIRE( r.clean() );
        REQUIRE( r.instances.empty() );
    }
}

} // namespace keel
#endif
