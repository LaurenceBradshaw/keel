// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/forward_borrows.h"
#include <algorithm>
#include <cassert>
#include "ir/simplify.h"

namespace keel
{
namespace
{
// The local each pointer can only point at, where the pointer never escapes. A pointer assigned
// once, a copy of another, points where that one does.
std::vector<Local_id> forwarded_to( const Function& func, const Type_table& table )
{
    std::vector<Local_id>         to( func.locals.size() );
    std::vector<Local_id>         copy_of( func.locals.size() );
    std::vector<bool>             refused( func.locals.size(), false );
    std::vector<const Statement*> assigned_by( func.locals.size(), nullptr );

    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign || !statement.place.is_whole_local() )
        {
            continue;
        }

        const u32     p     = statement.place.local.v;
        const Rvalue& value = func.rvalue( statement );
        const Type_id type  = func.locals[statement.place.local].type;
        const bool    borrow =
            value.kind == Rvalue_kind::Address_of && value.a.place.is_whole_local() &&
            ( value.address_purpose == Address_purpose::Borrow || value.address_purpose == Address_purpose::Read ) &&
            table.is_pointer( type ) && table.get( type ).element == func.locals[value.a.place.local].type;
        const bool copy = value.kind == Rvalue_kind::Use && value.a.has_place() && value.a.place.is_whole_local() &&
                          table.is_pointer( type ) && func.locals[value.a.place.local].type == type;

        if( ( !borrow && !copy ) || assigned_by[p] != nullptr )
        {
            refused[p] = true;
            continue;
        }

        ( borrow ? to[p] : copy_of[p] ) = value.a.place.local;
        assigned_by[p]                  = &statement;
    }

    // A whole `_p` anywhere but its own assignment, storage markers and a copy into another is the
    // pointer escaping.
    std::vector<u32> uses( func.locals.size(), 0 );
    for_each_place(
        func,
        [&]( const Place& place, const Statement* owner )
        {
            if( !place.is_whole_local() || assigned_by[place.local.v] == owner )
            {
                return;
            }

            const bool marker = owner != nullptr &&
                                ( owner->kind == Statement_kind::Storage_live || owner->kind == Statement_kind::Storage_dead );
            uses[place.local.v] += !marker;
        }
    );

    for( const Local_id q : copy_of )
    {
        if( q.is_valid() )
        {
            uses[q.v]--;
        }
    }

    for( u32 p = 0; p < uses.size(); ++p )
    {
        refused[p] = refused[p] || uses[p] > 0;
    }

    refused[k_return_slot.v] = true;

    // A copy is forwarded only with what it copies, and copying into one kept is an escape.
    for( bool changed = true; changed; )
    {
        changed = false;
        for( u32 p = 0; p < to.size(); ++p )
        {
            const Local_id q = copy_of[p];
            if( !q.is_valid() || refused[p] == refused[q.v] )
            {
                continue;
            }
            refused[p] = refused[q.v] = true;
            changed                   = true;
        }
    }

    // Each copy takes its source's target; a cycle of copies has none.
    for( bool changed = true; changed; )
    {
        changed = false;
        for( u32 p = 0; p < to.size(); ++p )
        {
            const Local_id q = copy_of[p];
            if( q.is_valid() && !to[p].is_valid() && to[q.v].is_valid() )
            {
                to[p]   = to[q.v];
                changed = true;
            }
        }
    }

    for( u32 p = 0; p < to.size(); ++p )
    {
        if( refused[p] )
        {
            to[p] = Local_id {};
        }
    }

    return to;
}
} // namespace

void forward_borrows( Function& func, const Type_table& table )
{
    const std::vector<Local_id> to = forwarded_to( func, table );

    if( std::ranges::none_of( to, []( Local_id k ) { return k.is_valid(); } ) )
    {
        return;
    }

    std::vector<bool> deleted( func.statements.size(), false );

    for( u32 s = 0; s < func.statements.size(); ++s )
    {
        const Place& place = func.statements[s].place;
        deleted[s]         = place.is_whole_local() && to[place.local.v].is_valid();
    }

    // `(*_p).f` is `_k.f`: drop the leading `Deref`.
    for_each_place(
        func,
        [&]( Place& place, const Statement* )
        {
            if( place.is_global() || !place.local.is_valid() || !to[place.local.v].is_valid() || place.num_projections == 0 )
            {
                return;
            }

            assert( func.projections[place.first_projection].kind == Projection_kind::Deref );
            place.local = to[place.local.v];
            ++place.first_projection;
            --place.num_projections;
        }
    );

    remove_statements( func, deleted );
}
} // namespace keel

#ifdef ENABLE_UNIT_TESTS
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
    "i32 look( i32* q ) { return 0; }\n"
    "i32 main() { return 0; }\n";

// Optimised, as a release build lowers it. The function under test is always the first.
struct Forwarded : Compiled
{
    explicit Forwarded( std::string_view source, std::optional<Inline_weights> inlining = Inline_weights {} )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build { .optimise = true, .inlining = inlining }
          )
    {
    }

    const Function& first() const
    {
        return functions.front();
    }

    std::size_t addresses() const
    {
        return static_cast<std::size_t>( std::ranges::count_if(
            first().statements,
            [&]( const Statement& s )
            { return s.kind == Statement_kind::Assign && first().rvalue( s ).kind == Rvalue_kind::Address_of; }
        ) );
    }

    std::size_t derefs() const
    {
        const Function& func  = first();
        std::size_t     count = 0;
        for_each_place( func, [&]( const Place& place, const Statement* ) { count += has_deref_projection( func, place ); } );
        return count;
    }
};

} // namespace

// The two `ref`s go, and what is left folds to the sum.
TEST_CASE( "forward_borrows_names_the_local_a_ref_points_at", "[opt][forward]" )
{
    for( const char* source : {
             "i32 f() { i32 n = 1; ref i32 r1 = n; ref i32 r2 = n; r1 = 2; n = 3; return r1 + r2; }\n",
             "i32 f( i32 a ) { i32 t = 0; i32 n = 0; while( n < a ) { i32 k = n; ref i32 r = k; r = r + 1; t = t + r; n++; } "
             "return t; }\n",
         } )
    {
        const Forwarded p( source );
        INFO( source );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.first() ).empty() );
        REQUIRE( p.addresses() == 0 );
        REQUIRE( p.derefs() == 0 );
    }
}

// An inlined method's `this` is a copy of the caller's pointer, which points at the same local.
TEST_CASE( "forward_borrows_names_the_local_a_copied_pointer_points_at", "[opt][forward]" )
{
    const Forwarded p( "i64 f() { Counter a = Counter(); Counter b = Counter(); a.both( ref b ); return a.n + b.n; }\n"
                       "class Counter { public i64 n; public Counter() { n = 1; } public void times( i64 k ) { n = n * k; }"
                       " public void both( ref Counter o ) { times( 2 ); this.times( 3 ); o.times( 5 ); } };\n" );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.addresses() == 0 );
    REQUIRE( p.derefs() == 0 );
}

// A borrow of a field, which only splitting makes a borrow of a whole local.
TEST_CASE( "forward_borrows_names_the_field_splitting_made_a_local", "[opt][forward]" )
{
    const Forwarded p(
        "i32 f() { Holder h = Holder(); return h.x() + deep( h ); }\n"
        "struct Pair { i32 x; i32 y; };\n"
        "class Holder { Pair p; public Holder() { p = Pair { 1, 2 }; } public const ref Pair get() const { return p; }"
        " public const ref i32 x() const { return get().x; } };\n"
        "const ref i32 deep( const ref Holder h ) { return h.get().x; }\n"
    );
    REQUIRE( p.clean() );
    REQUIRE( verify( p.first() ).empty() );
    REQUIRE( p.addresses() == 0 );
    REQUIRE( p.derefs() == 0 );
}

// Pointing at one of two, or passed on: each keeps its address.
TEST_CASE( "forward_borrows_keeps_a_pointer_that_may_point_elsewhere", "[opt][forward]" )
{
    for( const char* source : {
             "i32 f( i32 a ) { i32 x = 1; i32 y = 2; i32* p = &x; if( a > 0 ) { p = &y; } return *p; }\n",
         } )
    {
        const Forwarded p( source );
        INFO( source );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.first() ).empty() );
        REQUIRE( p.addresses() > 0 );
    }

    const Forwarded passed( "i32 f() { i32 n = 1; i32* p = &n; return look( p ); }\n", std::nullopt );
    REQUIRE( passed.clean() );
    REQUIRE( passed.addresses() == 1 );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
