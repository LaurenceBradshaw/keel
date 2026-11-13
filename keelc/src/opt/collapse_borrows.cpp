// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/collapse_borrows.h"

namespace keel
{
namespace
{
// Each local's one assignment, or null where it has none or more, or is addressed.
std::vector<const Statement*> only_assignments( const Function& func )
{
    std::vector<const Statement*> only( func.locals.size(), nullptr );
    std::vector<bool>             refused( func.locals.size(), false );

    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        const Rvalue& value = func.rvalue( statement );
        if( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() )
        {
            refused[value.a.place.local.v] = true;
        }

        if( !statement.place.is_global() )
        {
            const u32 p = statement.place.local.v;
            refused[p]  = refused[p] || only[p] != nullptr || !statement.place.is_whole_local();
            only[p]     = &statement;
        }
    }

    for( u32 p = 0; p < only.size(); ++p )
    {
        only[p] = refused[p] ? nullptr : only[p];
    }

    return only;
}

// The value `copy (*p)` reads, where `p = copy q as T*` and `q = &x`, with `x` a whole local and a
// builtin of `T`'s size: its bits, a cast, or `x` itself.
std::optional<Rvalue>
reinterpreted( const Function& func, const Rvalue& read, const std::vector<const Statement*>& only, const Type_table& table )
{
    const auto assigned = [&]( const Place& place ) -> const Rvalue*
    {
        if( place.is_global() || !place.is_whole_local() || only[place.local.v] == nullptr )
        {
            return nullptr;
        }

        return &func.rvalue( *only[place.local.v] );
    };

    const Place& place = read.a.place;
    if( read.kind != Rvalue_kind::Use || !read.a.has_place() || place.is_global() || place.num_projections != 1 ||
        func.projections[place.first_projection].kind != Projection_kind::Deref )
    {
        return std::nullopt;
    }

    const Rvalue* cast = assigned( Place { .local = place.local } );
    if( cast == nullptr || cast->kind != Rvalue_kind::Cast || !cast->a.has_place() )
    {
        return std::nullopt;
    }

    const Rvalue* address = assigned( cast->a.place );
    if( address == nullptr || address->kind != Rvalue_kind::Address_of || address->a.place.is_global() ||
        !address->a.place.is_whole_local() )
    {
        return std::nullopt;
    }

    const Local_id x       = address->a.place.local;
    const Type_id  from    = func.locals[x].type;
    const Type_id  to      = read.a.type;
    const Type&    f       = table.get( from );
    const Type&    t       = table.get( to );
    const auto     builtin = []( const Type& type ) { return type.kind == Type_kind::Int || type.kind == Type_kind::Float; };
    if( !builtin( f ) || !builtin( t ) || f.width != t.width )
    {
        return std::nullopt;
    }

    const Operand value = copy( Place { .local = x }, from );
    if( from == to )
    {
        return use( value );
    }

    return f.kind == t.kind ? cast_to( value, to ) : bits_of( value, to );
}
} // namespace

void collapse_borrows( Function& func, const Type_table& table )
{
    const std::vector<const Statement*> only = only_assignments( func );
    for( Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        if( std::optional<Rvalue> value = reinterpreted( func, func.rvalue( statement ), only, table ) )
        {
            func.rvalue( statement ) = *value;
        }
    }

    for( Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign || func.rvalue( statement ).kind != Rvalue_kind::Address_of )
        {
            continue;
        }

        const Place& place = func.rvalue( statement ).a.place;

        // Only a local's own pointee: `&(*_1).f` is a field's address, and a deeper pointer's type needs the AST.
        if( place.is_global() || place.num_projections != 1 ||
            func.projections[place.first_projection].kind != Projection_kind::Deref )
        {
            continue;
        }

        const Type_id pointer = func.locals[place.local].type;

        // A class's pointer becoming an interface's builds a pair; `const` aside, the two must match.
        if( !table.is_pointer( pointer ) || !table.is_pointer( func.rvalue( statement ).type ) ||
            table.get( pointer ).element != table.get( func.rvalue( statement ).type ).element )
        {
            continue;
        }

        func.rvalue( statement ) = use( copy( Place { .local = place.local }, pointer ) );
    }
}
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <algorithm>
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

constexpr std::string_view k_types =
    "interface shape { i32 area() const; };\n"
    "class Box implements shape { public i32 v; public Box( i32 n ) { v = n; } public i32 area() const { return v; } };\n"
    "class Pair { public Box a; public Pair() { a = Box( 1 ); } };\n"
    "i32 peek( const ref Box b ) { return b.v; }\n"
    "i32 measure( const ref shape s ) { return s.area(); }\n"
    "i32 main() { return 0; }\n";

// Optimised, as a release build lowers it. The function under test is always the first.
struct Collapsed : Compiled
{
    explicit Collapsed( std::string_view source, std::optional<Inline_weights> inlining = Inline_weights {} )
        : Compiled(
              std::string( source ) + std::string( k_types ), Stage::Elaborated, Build { .level = 2, .inlining = inlining }
          )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    // Addresses taken through a pointer, `&(*_1)` and `&(*_1).f` alike.
    std::size_t deref_borrows() const
    {
        const Function& func = first();
        return static_cast<std::size_t>( std::ranges::count_if(
            func.statements,
            [&]( const Statement& s )
            {
                const Rvalue& value = func.rvalue( s );

                return s.kind == Statement_kind::Assign && value.kind == Rvalue_kind::Address_of &&
                       has_deref_projection( func, value.a.place );
            }
        ) );
    }
};

} // namespace

TEST_CASE( "collapse_borrows_copies_the_pointer_a_deref_borrows", "[opt][collapse]" )
{
    const Collapsed p( "i32 f( const ref Box b ) { return peek( b ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.deref_borrows() == 0 );
}

// The interface's pair is a Make_interface of the pointer, so the `&(*b)` under it collapses to
// `b`. Not inlined, so the call is not made direct and the pair stays.
TEST_CASE( "collapse_borrows_keeps_a_class_pointer_becoming_an_interface_one", "[opt][collapse]" )
{
    const Collapsed p( "i32 f( const ref Box b ) { return measure( b ); }\n", std::nullopt );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.deref_borrows() == 0 );
    REQUIRE( std::ranges::any_of(
        p.first().statements, [&]( const Statement& s ) { return p.first().rvalue( s ).kind == Rvalue_kind::Make_interface; }
    ) );
}

TEST_CASE( "collapse_borrows_keeps_a_field_through_a_pointer", "[opt][collapse]" )
{
    const Collapsed p( "i32 f( const ref Pair p ) { return peek( p.a ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.deref_borrows() == 1 );
}

// Not inlined, so `f` keeps its own read.
TEST_CASE( "collapse_borrows_reads_a_cast_float_as_its_bits", "[opt][collapse]" )
{
    const Collapsed p( "u64 f( f64 x ) { unsafe { return *cast<u64*>( &x ); } }\n", std::nullopt );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    const auto has = [&]( Rvalue_kind kind )
    {
        return std::ranges::any_of(
            p.first().statements, [&]( const Statement& s ) { return p.first().rvalue( s ).kind == kind; }
        );
    };
    REQUIRE( has( Rvalue_kind::Bits ) );
    REQUIRE( !has( Rvalue_kind::Address_of ) );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
