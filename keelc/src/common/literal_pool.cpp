// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/literal_pool.h"

#include <cassert>

namespace keel
{

Literal_pool::Literal_pool()
{
    entries_.push_back( std::monostate() ); // slot 0 is reserved
}

Literal_id Literal_pool::add_integer( u64 magnitude )
{
    entries_.push_back( magnitude );
    return Literal_id { narrow_cast<u32>( entries_.size() - 1 ) };
}

Literal_id Literal_pool::add_float( f64 value )
{
    entries_.push_back( value );
    return Literal_id { narrow_cast<u32>( entries_.size() - 1 ) };
}

Literal_id Literal_pool::add_string( std::string bytes )
{
    entries_.push_back( std::move( bytes ) );
    return Literal_id { narrow_cast<u32>( entries_.size() - 1 ) };
}

u64 Literal_pool::integer( Literal_id id ) const
{
    assert( id.is_valid() && id.v < entries_.size() && kind( id ) == Literal_kind::Integer && "invalid Literal_id" );
    return std::get<u64>( entries_[id.v] );
}

f64 Literal_pool::floating( Literal_id id ) const
{
    assert( id.is_valid() && id.v < entries_.size() && kind( id ) == Literal_kind::Float && "invalid Literal_id" );
    return std::get<f64>( entries_[id.v] );
}

std::string_view Literal_pool::string( Literal_id id ) const
{
    assert( id.is_valid() && id.v < entries_.size() && kind( id ) == Literal_kind::String && "invalid Literal_id" );
    return std::string_view( std::get<std::string>( entries_[id.v] ) );
}

Literal_kind Literal_pool::kind( Literal_id id ) const
{
    assert( id.is_valid() && id.v < entries_.size() && "invalid Literal_id" );

    return Literal_kind( entries_[id.v].index() - 1 );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{

TEST_CASE( "literal_pool_round_trips_values", "[common][literals]" )
{
    Literal_pool pool;

    REQUIRE_FALSE( Literal_id {}.is_valid() );

    const Literal_id small = pool.add_integer( 42 );
    const Literal_id zero  = pool.add_integer( 0 );
    const Literal_id big   = pool.add_integer( 18446744073709551615ull );

    REQUIRE( small.is_valid() );
    REQUIRE( zero.is_valid() ); // a zero *value* still gets a valid handle
    REQUIRE( pool.integer( small ) == 42 );
    REQUIRE( pool.integer( zero ) == 0 );
    REQUIRE( pool.integer( big ) == 18446744073709551615ull );

    SECTION( "equal values are separate entries - this records, it does not intern" )
    {
        REQUIRE( pool.add_integer( 42 ) != small );
    }

    SECTION( "a float leaves the integers alone" )
    {
        const Literal_id f = pool.add_float( 1.5 );

        REQUIRE( pool.floating( f ) == 1.5 );
        REQUIRE( pool.integer( small ) == 42 ); // unmoved by the float
    }

    // A literal's size counts the bytes, so a zero inside one is one of them.
    SECTION( "strings keep every byte, a zero included" )
    {
        const Literal_id s     = pool.add_string( std::string( "a\0b", 3 ) );
        const Literal_id empty = pool.add_string( "" );

        REQUIRE( pool.string( s ) == std::string_view( "a\0b", 3 ) );
        REQUIRE( pool.string( empty ).empty() );
        REQUIRE( pool.integer( small ) == 42 ); // unmoved by the strings
    }

    SECTION( "handles stay valid as the pool grows" )
    {
        for( int i = 0; i < 1000; ++i )
        {
            pool.add_integer( static_cast<u64>( i ) );
        }

        REQUIRE( pool.integer( small ) == 42 );
        REQUIRE( pool.integer( big ) == 18446744073709551615ull );
    }
}

TEST_CASE( "literal_pool_ids_name_their_kind", "[common][literals]" )
{
    Literal_pool pool;

    const Literal_id i = pool.add_integer( 1 );
    const Literal_id f = pool.add_float( 1.0 );
    const Literal_id s = pool.add_string( "1" );

    REQUIRE( pool.kind( i ) == Literal_kind::Integer );
    REQUIRE( pool.kind( f ) == Literal_kind::Float );
    REQUIRE( pool.kind( s ) == Literal_kind::String );

    // Each kind used to count from 1, so the first of each shared an id.
    REQUIRE( i != f );
    REQUIRE( f != s );
    REQUIRE( i != s );
}

} // namespace keel
#endif
