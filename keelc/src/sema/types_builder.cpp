#include "sema/types_builder.h"

namespace keel::sema
{

void Types_builder::size_to( std::size_t node_count )
{
    types_.assign( node_count, Type_id {} );
}

Type_id Types_builder::record( Node_id id, Type_id type )
{
    types_[id.v] = type;

    return type;
}

Type_id Types_builder::type_of( Node_id id ) const
{
    return id.v < types_.size() ? types_[id.v] : Type_id {};
}

std::span<const Type_id> Types_builder::recorded() const
{
    return types_;
}

Type_table Types_builder::take_table()
{
    return std::move( table_ );
}

std::vector<Type_id> Types_builder::take_types()
{
    return std::move( types_ );
}

} // namespace keel::sema

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keel
{
namespace
{

TEST_CASE( "types_builder_records_and_reads_back_a_node_type", "[sema][types]" )
{
    sema::Types_builder builder;
    builder.size_to( 4 );

    const Type_id i32 = builder.table().integer( 32, true );

    REQUIRE( builder.record( Node_id { 2 }, i32 ) == i32 );
    REQUIRE( builder.type_of( Node_id { 2 } ) == i32 );
    REQUIRE( !builder.type_of( Node_id { 1 } ).is_valid() ); // never written
}

TEST_CASE( "types_builder_answers_invalid_for_a_node_past_the_end", "[sema][types]" )
{
    sema::Types_builder builder;
    builder.size_to( 2 );

    // The signature pass asks about nodes the walk has not reached; out of range is the same
    // answer as untyped, which is what lets the callers drop their own bounds tests.
    REQUIRE( !builder.type_of( Node_id { 9 } ).is_valid() );
    REQUIRE( !builder.type_of( Node_id {} ).is_valid() );
}

TEST_CASE( "types_builder_hands_over_its_state_and_keeps_none", "[sema][types]" )
{
    sema::Types_builder builder;
    builder.size_to( 3 );
    builder.record( Node_id { 1 }, builder.table().integer( 8, false ) );

    REQUIRE( builder.take_types().size() == 3 );
    REQUIRE( builder.recorded().empty() );
}

} // namespace
} // namespace keel
#endif
