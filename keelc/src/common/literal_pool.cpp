#include "common/literal_pool.h"

#include <cassert>

namespace keel
{

Literal_pool::Literal_pool()
{
    // Dummy values in slot 0
    integers_.push_back( {} );
    floats_.push_back( {} );
}

Literal_id Literal_pool::add_integer( u64 magnitude )
{
    integers_.push_back( magnitude );
    return Literal_id { narrow_cast<u32>( integers_.size() - 1 ) };
}

Literal_id Literal_pool::add_float( f64 value )
{
    floats_.push_back( value );
    return Literal_id { narrow_cast<u32>( floats_.size() - 1 ) };
}

u64 Literal_pool::integer( Literal_id id ) const
{
    assert( id.is_valid() && id.v < integers_.size() && "invalid Literal_id" );
    return integers_[id.v];
}

f64 Literal_pool::floating( Literal_id id ) const
{
    assert( id.is_valid() && id.v < floats_.size() && "invalid Literal_id" );
    return floats_[id.v];
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

    SECTION( "integers and floats have independent id spaces" )
    {
        const Literal_id f = pool.add_float( 1.5 );

        REQUIRE( pool.floating( f ) == 1.5 );
        REQUIRE( pool.integer( small ) == 42 ); // unmoved by the float
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

} // namespace keel
#endif
