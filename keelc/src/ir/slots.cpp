// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/slots.h"

namespace keel
{

Slot_map::Slot_map( const Function& func )
    : func_( func ),
      base_( narrow_cast<u32>( func.locals.size() ) ),
      run_first_( func.locals.size(), 0 ),
      run_last_( func.locals.size(), 0 ),
      through_( func.locals.size(), 0 )
{
    for( u32 i = narrow_cast<u32>( func.fields.size() ); i-- > 0; )
    {
        const u32 local = func.fields[i].local.v;

        if( run_last_[local] == 0 )
        {
            run_last_[local] = i + 1;
        }

        run_first_[local] = i;
    }

    for( const Local_id local : func.out_parameters )
    {
        through_[local.v] = 1;
    }

    if( func.constructed.is_valid() )
    {
        through_[func.constructed.v] = 1;
    }
}

Slots Slot_map::of( const Place& place ) const
{
    assert( !place.is_global() );

    const u32 local = place.local.v;
    assert( local < base_ && "a local appended after the map was built" );
    u32 i = 0;

    if( through_[local] != 0 && place.num_projections > 0 &&
        func_.projections[place.first_projection].kind == Projection_kind::Deref )
    {
        i = 1;
    }

    Slots slots = under( local );

    for( ; i < place.num_projections; ++i )
    {
        const Projection& projection = func_.projections[place.first_projection + i];

        if( projection.kind != Projection_kind::Field || is_leaf( slots.node ) )
        {
            slots.exact   = false;
            slots.element = projection.kind == Projection_kind::Index;
            return slots;
        }

        u32 child = slots.first;

        while( child < slots.last && func_.fields[child - base_].field != projection.field )
        {
            child = next( child );
        }

        assert( child < slots.last && "a field the table does not record" );

        slots = under( child );
    }

    return slots;
}

Slots Slot_map::under( u32 slot ) const
{
    if( is_leaf( slot ) )
    {
        return Slots { .node = slot, .first = slot, .last = slot + 1 };
    }

    return Slots { .node = slot, .first = slot < base_ ? base_ + run_first_[slot] : slot + 1, .last = next( slot ) };
}

bool Slot_map::is_leaf( u32 slot ) const
{
    return slot < base_ ? run_last_[slot] == 0 : func_.fields[slot - base_].end == slot - base_ + 1;
}

u32 Slot_map::next( u32 slot ) const
{
    if( slot < base_ )
    {
        return run_last_[slot] == 0 ? slot + 1 : base_ + run_last_[slot];
    }

    return base_ + func_.fields[slot - base_].end;
}

Local_id Slot_map::local( u32 slot ) const
{
    return slot < base_ ? Local_id { slot } : func_.fields[slot - base_].local;
}

u32 Slot_map::field( u32 slot ) const
{
    return slot < base_ ? k_no_field : slot - base_;
}

u32 Slot_map::parent( u32 slot ) const
{
    if( slot < base_ )
    {
        return k_no_field;
    }

    const Field_slot& entry = func_.fields[slot - base_];
    return entry.parent == k_no_field ? entry.local.v : base_ + entry.parent;
}

bool Slot_map::built_whole( u32 slot ) const
{
    return slot < base_ ? func_.locals[slot].built_whole : func_.fields[slot - base_].built_whole;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <algorithm>
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

constexpr std::string_view k_source =
    "struct In { i32 x; i32 y; };\n"
    "struct P { In inner; i32 z; };\n"
    "class Box { public i64 value; public Box( i64 v ) { value = v; } ~Box() { } };\n"
    "class Q { public Box b; public i32 n; };\n"
    "void f( out P o ) { P p = P { In { 1, 2 }, 3 }; Box c = Box( 1 ); Q q = Q { Box( 2 ), 4 };"
    " o = p; }\n"
    "i32 main() { return 0; }\n";

struct Mapped : Compiled
{
    Mapped()
        : Compiled( k_source, Stage::Lowered )
    {
    }

    // A copy, so a test can append the projections of the places it builds.
    Function named( std::string_view name ) const
    {
        const Symbol_id symbol = interner.find( name );
        const auto      found =
            std::ranges::find_if( all, [&]( const Function& f ) { return ast.name( f.declaration ) == symbol; } );
        REQUIRE( found != all.end() );
        return *found;
    }

    Function constructor_of( std::string_view aggregate ) const
    {
        const Symbol_id symbol = interner.find( aggregate );
        const auto      found  = std::ranges::find_if(
            all,
            [&]( const Function& f )
            { return f.constructed.is_valid() && ast.name( ast.enclosing_aggregate( f.declaration ) ) == symbol; }
        );
        REQUIRE( found != all.end() );
        return *found;
    }

    Local_id local( const Function& func, std::string_view name ) const
    {
        const Symbol_id symbol = interner.find( name );
        for( u32 i = 0; i < func.locals.size(); ++i )
        {
            if( func.locals[i].name == symbol )
            {
                return Local_id { i };
            }
        }
        FAIL( "no local " << name );
        return {};
    }

    // `local.a.b`, or `(*local).a.b` when `deref`, its projections appended to `func`.
    Place path( Function& func, Local_id local, bool deref, std::initializer_list<std::string_view> names ) const
    {
        const u32 first = narrow_cast<u32>( func.projections.size() );
        if( deref )
        {
            func.projections.push_back( Projection { .kind = Projection_kind::Deref } );
        }

        u32 parent = k_no_field;
        for( const std::string_view name : names )
        {
            const Symbol_id symbol = interner.find( name );
            u32             i      = 0;
            while( i < func.fields.size() && !( func.fields[i].local == local && func.fields[i].parent == parent &&
                                                ast.name( func.fields[i].field ) == symbol ) )
            {
                ++i;
            }
            REQUIRE( i < func.fields.size() );
            func.projections.push_back( Projection { .kind = Projection_kind::Field, .field = func.fields[i].field } );
            parent = i;
        }

        return Place {
            .local = local, .first_projection = first, .num_projections = narrow_cast<u32>( func.projections.size() ) - first
        };
    }
};

} // namespace

TEST_CASE( "slot_map_splits_a_struct_local_into_a_preorder_tree", "[ir][slots]" )
{
    const Mapped   mapped;
    Function       func = mapped.named( "f" );
    const Local_id p    = mapped.local( func, "p" );
    const Place    whole { .local = p };
    const Place    inner_place = mapped.path( func, p, false, { "inner" } );
    const Place    y_place     = mapped.path( func, p, false, { "inner", "y" } );
    const Place    z_place     = mapped.path( func, p, false, { "z" } );
    const Slot_map slots( func );

    const Slots all = slots.of( whole );
    REQUIRE( all.node == p.v );
    REQUIRE_FALSE( slots.is_leaf( p.v ) );
    REQUIRE( all.last - all.first == 4 ); // inner, inner.x, inner.y, z

    const Slots inner = slots.of( inner_place );
    REQUIRE( inner.exact );
    REQUIRE_FALSE( slots.is_leaf( inner.node ) );
    REQUIRE( inner.first == inner.node + 1 );
    REQUIRE( inner.last == inner.node + 3 );
    REQUIRE( slots.parent( inner.node ) == p.v );

    const Slots y = slots.of( y_place );
    REQUIRE( y.exact );
    REQUIRE( slots.is_leaf( y.node ) );
    REQUIRE( y.first == y.node );
    REQUIRE( y.last == y.node + 1 );
    REQUIRE( slots.parent( y.node ) == inner.node );
    REQUIRE( slots.local( y.node ) == p );
    REQUIRE( mapped.ast.name( func.fields[slots.field( y.node )].field ) == mapped.interner.find( "y" ) );

    REQUIRE( slots.next( inner.node ) == slots.of( z_place ).node );
    REQUIRE( slots.next( p.v ) == all.last );
    REQUIRE( slots.field( p.v ) == k_no_field );
    REQUIRE( slots.parent( p.v ) == k_no_field );
    REQUIRE( is_field_path( func, y_place ) );
}

TEST_CASE( "slot_map_tracks_a_class_with_a_destructor_whole", "[ir][slots]" )
{
    const Mapped   mapped;
    Function       func = mapped.named( "f" );
    const Local_id c    = mapped.local( func, "c" );
    const Local_id q    = mapped.local( func, "q" );
    const Local_id p    = mapped.local( func, "p" );

    const Place value_place {
        .local = c, .first_projection = narrow_cast<u32>( func.projections.size() ), .num_projections = 1
    };
    func.projections.push_back(
        Projection { .kind = Projection_kind::Field, .field = mapped.constructor_of( "Box" ).fields[0].field }
    );
    const Place    b_place = mapped.path( func, q, false, { "b" } );
    const Slot_map slots( func );

    REQUIRE( slots.is_leaf( c.v ) );
    REQUIRE( slots.built_whole( c.v ) );
    REQUIRE_FALSE( slots.built_whole( p.v ) );

    // `c.value` goes past a node tracked whole.
    const Slots value = slots.of( value_place );
    REQUIRE_FALSE( value.exact );
    REQUIRE( value.node == c.v );

    const Slots b = slots.of( b_place );
    REQUIRE( b.exact );
    REQUIRE( slots.is_leaf( b.node ) );
    REQUIRE( slots.built_whole( b.node ) );
    REQUIRE( func.fields[slots.field( b.node )].is_owning );
}

TEST_CASE( "slot_map_describes_an_out_parameters_referent", "[ir][slots]" )
{
    const Mapped   mapped;
    Function       func    = mapped.named( "f" );
    const Local_id o       = mapped.local( func, "o" );
    const Place    x_place = mapped.path( func, o, true, { "inner", "x" } );
    const Slot_map slots( func );

    const Slots x = slots.of( x_place );
    REQUIRE( x.exact );
    REQUIRE( slots.is_leaf( x.node ) );
    REQUIRE( slots.local( x.node ) == o );
    REQUIRE( slots.of( Place { .local = o } ).last - slots.of( Place { .local = o } ).first == 4 );
    REQUIRE_FALSE( is_field_path( func, x_place ) );
}

TEST_CASE( "slot_map_splits_a_constructors_object_one_level_despite_its_destructor", "[ir][slots]" )
{
    const Mapped   mapped;
    const Function func = mapped.constructor_of( "Box" );
    const Slot_map slots( func );

    REQUIRE( func.fields.size() == 1 );
    REQUIRE( func.fields[0].local == func.constructed );
    REQUIRE_FALSE( func.fields[0].is_owning );
    REQUIRE_FALSE( slots.is_leaf( func.constructed.v ) );
    REQUIRE( slots.built_whole( func.constructed.v ) );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
