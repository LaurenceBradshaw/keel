// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/layout.h"
#include <algorithm>
#include <cassert>

namespace keel
{
namespace
{
u64 align_up( u64 value, u64 align )
{
    return ( value + align - 1 ) / align * align;
}
} // namespace

Layout::Layout( const Description& program, const Type_table& table, Target target )
    : program_( program ),
      table_( table ),
      target_( target )
{
}

u64 Layout::size( Type_id type ) const
{
    return entry( type ).size;
}

u64 Layout::align( Type_id type ) const
{
    return entry( type ).align;
}

u64 Layout::offset( Type_id aggregate, Node_id field ) const
{
    const Entry& found = entry( aggregate );
    const auto   at    = std::ranges::find( found.fields, field );
    assert( at != found.fields.end() && "the aggregate holds this field" );
    return found.offsets[static_cast<std::size_t>( at - found.fields.begin() )];
}

u64 Layout::payload( Type_id type ) const
{
    return entry( type ).payload;
}

const Layout::Entry& Layout::entry( Type_id type ) const
{
    // A reference into an unordered_map survives the inserts a nested compute makes.
    if( const auto found = entries_.find( type.v ); found != entries_.end() )
    {
        return found->second;
    }
    Entry computed = compute( type );
    return entries_.emplace( type.v, std::move( computed ) ).first->second;
}

Layout::Entry Layout::scalar( u64 bytes ) const
{
    return Entry { .size = bytes, .align = std::min( bytes, target_.max_scalar_align ) };
}

Layout::Entry Layout::compute( Type_id type ) const
{
    const Type& described = table_.get( type );
    const Entry word      = Entry { .size = target_.pointer_size, .align = target_.pointer_align };

    // Fields placed in order from `start`, each `void` one taking nothing.
    const auto place = [&]( Entry& into, std::span<const Held_field> fields, u64 start )
    {
        u64 at = start;
        for( const Held_field& field : fields )
        {
            const Entry& held = entry( field.type );
            at                = align_up( at, held.align );
            into.fields.push_back( field.declaration );
            into.offsets.push_back( at );
            at += held.size;
            into.align = std::max( into.align, held.align );
        }
        return at;
    };

    // A tag, then a union of `arms`, each laid out as a struct from the union's start.
    const auto tagged = [&]( std::span<const std::span<const Held_field>> arms, std::span<const Type_id> members )
    {
        const Entry& tag = entry( described.element );
        Entry        out { .align = tag.align };

        u64 arm_align = 1;
        for( const auto arm : arms )
        {
            for( const Held_field& field : arm )
            {
                arm_align = std::max( arm_align, entry( field.type ).align );
            }
        }
        for( const Type_id member : members )
        {
            arm_align = std::max( arm_align, entry( member ).align );
        }

        out.payload = align_up( tag.size, arm_align );
        u64 end     = out.payload;
        for( const auto arm : arms )
        {
            end = std::max( end, place( out, arm, out.payload ) );
        }
        for( const Type_id member : members )
        {
            end = std::max( end, out.payload + entry( member ).size );
        }
        out.align = std::max( out.align, arm_align );
        out.size  = align_up( end, out.align );
        return out;
    };

    switch( described.kind )
    {
    case Type_kind::Void:
    case Type_kind::Never:
        return Entry {};

    case Type_kind::Bool:
        return scalar( 1 );

    case Type_kind::Int:
    case Type_kind::Float:
        return scalar( described.width / 8u );

    case Type_kind::Many_pointer:
    case Type_kind::Function:
    case Type_kind::Field:
        return word;

    // An interface reference is its object and its vtable; a run reference its data and count.
    case Type_kind::Pointer:
        if( table_.is_interface( described.element ) || table_.is_run( described.element ) )
        {
            return Entry { .size = 2 * target_.pointer_size, .align = target_.pointer_align };
        }
        return word;

    case Type_kind::Array:
    {
        const Entry& element = entry( described.element );
        return Entry { .size = element.size * described.count, .align = element.align };
    }

    case Type_kind::Union:
        return tagged( {}, described.arguments );

    case Type_kind::Enum:
    {
        if( !program_[described.declaration].payload )
        {
            return entry( described.element );
        }

        // Each variant's fields, which arrive variant by variant.
        const std::span<const Held_field>        fields = program_.fields( type );
        std::vector<std::span<const Held_field>> arms;
        for( std::size_t i = 0; i < fields.size(); )
        {
            std::size_t j = i;
            while( j < fields.size() && fields[j].variant == fields[i].variant )
            {
                ++j;
            }
            arms.push_back( fields.subspan( i, j - i ) );
            i = j;
        }
        return tagged( arms, {} );
    }

    case Type_kind::Struct:
    case Type_kind::Closure:
    {
        Entry     out;
        const u64 end = place( out, program_.fields( type ), 0 );
        out.size      = end == 0 ? 1 : align_up( end, out.align ); // D51: one byte for nothing
        return out;
    }

    default:
        assert( false && "no layout for this type" );
        return Entry {};
    }
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>
#include "check/pipeline_test_support.h"

namespace keel
{

// x86-64's own layout: padding before a wider field, a tag then a union of variants, two-word
// references, one byte for an instance of nothing.
TEST_CASE( "layout_places_fields_as_x86_64_does", "[codegen][layout]" )
{
    Compiled p(
        "class Pair { public u8 a; public i64 b; public u16 c; public Pair() { a = 1; b = 2; c = 3; } };\n"
        "class Box<T> { public T held; public Box( move T h ) { held = move h; } };\n"
        "void none() {}\n"
        "enum Shape { Dot, Square( i32 side ), Line( u8 tip, f64 length ) };\n"
        "i32 main() { Pair p = Pair(); Box<void> v = Box<void>( none() ); Shape s = Shape::Square( 2 ); "
        "switch( s ) { case Shape::Dot: return 1; case Shape::Square( side ): return side - 2; "
        "case Shape::Line( tip, length ): return 3; } }\n",
        Stage::Elaborated
    );
    INFO( p.rendered() );
    REQUIRE( p.clean() );

    const Description description = describe( p.ast, p.types, p.interner );
    const Type_table& table       = p.types.table();
    const Layout      layout( description, table );

    const auto instance = [&]( std::string_view name )
    {
        for( const Type_id type : table.composite_types() )
        {
            if( table.name( type ) == name )
            {
                return type;
            }
        }
        return Type_id {};
    };

    const Type_id pair = instance( "Pair" );
    REQUIRE( layout.size( pair ) == 24 );
    REQUIRE( layout.align( pair ) == 8 );
    REQUIRE( layout.offset( pair, description.field_named( pair, "b" )->declaration ) == 8 );
    REQUIRE( layout.offset( pair, description.field_named( pair, "c" )->declaration ) == 16 );

    REQUIRE( layout.size( instance( "Box<void>" ) ) == 1 );

    // A u8 tag, then the widest variant, `Line`'s u8 and f64, from 8.
    const Type_id shape = instance( "Shape" );
    REQUIRE( layout.payload( shape ) == 8 );
    REQUIRE( layout.size( shape ) == 24 );
    REQUIRE( layout.offset( shape, description.field_named( shape, "side" )->declaration ) == 8 );
    REQUIRE( layout.offset( shape, description.field_named( shape, "length" )->declaration ) == 16 );

    REQUIRE( layout.size( table.integer( 16, false ) ) == 2 );
    REQUIRE( layout.size( table.builtin( Type_kind::Void ) ) == 0 );
}

} // namespace keel
#endif
