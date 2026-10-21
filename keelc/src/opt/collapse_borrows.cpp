// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/collapse_borrows.h"

namespace keel
{
void collapse_borrows( Function& func, const Type_table& table )
{
    for( Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign || statement.value.kind != Rvalue_kind::Address_of )
        {
            continue;
        }

        const Place& place = statement.value.a.place;

        // Only a local's own pointee: `&(*_1).f` is a field's address, and a deeper pointer's type needs the AST.
        if( place.is_global() || place.num_projections != 1 ||
            func.projections[place.first_projection].kind != Projection_kind::Deref )
        {
            continue;
        }

        const Type_id pointer = func.locals[place.local.v].type;

        // A class's pointer becoming an interface's builds a pair; `const` aside, the two must match.
        if( !table.is_pointer( pointer ) || !table.is_pointer( statement.value.type ) ||
            table.get( pointer ).element != table.get( statement.value.type ).element )
        {
            continue;
        }

        statement.value = use( copy( Place { .local = place.local }, pointer ) );
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
    explicit Collapsed( std::string_view source )
        : Compiled( std::string( source ) + std::string( k_types ), Stage::Elaborated, Build { .optimise = true } )
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
                return s.kind == Statement_kind::Assign && s.value.kind == Rvalue_kind::Address_of &&
                       has_deref_projection( func, s.value.a.place );
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

// Its `&` builds the interface's pair, which is not the pointer.
TEST_CASE( "collapse_borrows_keeps_a_class_pointer_becoming_an_interface_one", "[opt][collapse]" )
{
    const Collapsed p( "i32 f( const ref Box b ) { return measure( b ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.deref_borrows() == 1 );
}

TEST_CASE( "collapse_borrows_keeps_a_field_through_a_pointer", "[opt][collapse]" )
{
    const Collapsed p( "i32 f( const ref Pair p ) { return peek( p.a ); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.deref_borrows() == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
