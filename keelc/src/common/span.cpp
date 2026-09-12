// Span is header-only; this file exists only to host its tests. Empty TU in the keelc build.

#ifdef ENABLE_UNIT_TESTS
#include "common/span.h"

#include <catch2/catch_test_macros.hpp>

namespace keel
{
namespace
{

constexpr File_id k_file { 3 };
constexpr File_id k_other_file { 4 };

Span at( u32 start, u32 end )
{
    return Span { k_file, start, end };
}

} // namespace

TEST_CASE( "span_defaults_to_invalid", "[common][span]" )
{
    const Span s {};
    REQUIRE_FALSE( s.is_valid() );
    REQUIRE( s.is_empty() );
    REQUIRE( s == Span::none() );
}

TEST_CASE( "span_length_and_emptiness", "[common][span]" )
{
    REQUIRE( at( 10, 13 ).len() == 3 );
    REQUIRE_FALSE( at( 10, 13 ).is_empty() );

    const Span point = Span::point( k_file, 10 );
    REQUIRE( point.len() == 0 );
    REQUIRE( point.is_empty() );
    REQUIRE( point.is_valid() );
}

TEST_CASE( "span_merge_covers_both_operands", "[common][span]" )
{
    SECTION( "disjoint" )
    {
        REQUIRE( Span::merge( at( 0, 3 ), at( 10, 12 ) ) == at( 0, 12 ) );
    }
    SECTION( "adjacent" )
    {
        REQUIRE( Span::merge( at( 0, 3 ), at( 3, 6 ) ) == at( 0, 6 ) );
    }
    SECTION( "overlapping" )
    {
        REQUIRE( Span::merge( at( 0, 5 ), at( 3, 9 ) ) == at( 0, 9 ) );
    }
    SECTION( "nested" )
    {
        REQUIRE( Span::merge( at( 0, 20 ), at( 5, 9 ) ) == at( 0, 20 ) );
    }
    SECTION( "reversed argument order gives the same result" )
    {
        REQUIRE( Span::merge( at( 10, 12 ), at( 0, 3 ) ) == at( 0, 12 ) );
    }
    SECTION( "zero-length operand still widens" )
    {
        REQUIRE( Span::merge( at( 4, 4 ), at( 8, 9 ) ) == at( 4, 9 ) );
    }
}

TEST_CASE( "span_merge_absorbs_invalid_operands", "[common][span]" )
{
    REQUIRE( Span::merge( Span::none(), at( 2, 5 ) ) == at( 2, 5 ) );
    REQUIRE( Span::merge( at( 2, 5 ), Span::none() ) == at( 2, 5 ) );
    REQUIRE( Span::merge( Span::none(), Span::none() ) == Span::none() );
}

TEST_CASE( "span_contains", "[common][span]" )
{
    const Span outer = at( 10, 20 );

    REQUIRE( outer.contains( outer ) );
    REQUIRE( outer.contains( at( 12, 18 ) ) );
    REQUIRE( outer.contains( at( 10, 11 ) ) );
    REQUIRE( outer.contains( Span::point( k_file, 20 ) ) );

    REQUIRE_FALSE( outer.contains( at( 9, 15 ) ) );
    REQUIRE_FALSE( outer.contains( at( 15, 21 ) ) );
    REQUIRE_FALSE( outer.contains( Span { k_other_file, 12, 18 } ) );
}

TEST_CASE( "file_id_validity", "[common][span]" )
{
    REQUIRE( k_file.is_valid() );
    REQUIRE_FALSE( File_id {}.is_valid() );
    REQUIRE( File_id { 0 }.is_valid() );
}

} // namespace keel
#endif // ENABLE_UNIT_TESTS
