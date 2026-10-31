// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Id_vector is header-only; this file exists only to host its tests. Empty TU in the keelc build.

#ifdef ENABLE_UNIT_TESTS
#include "common/id_vector.h"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace keel
{
namespace
{
struct Apple_id
{
    u32 v = 0;
};

struct Pear_id
{
    u32 v = 0;
};

template <typename Table, typename Id>
concept indexable_by = requires( Table table, Id id ) { table[id]; };
} // namespace

TEST_CASE( "id_vector_takes_only_its_own_id", "[common][id_vector]" )
{
    Id_vector<Apple_id, int> apples;
    apples.push_back( 10 );
    const Apple_id second = apples.push_back( 20 );

    REQUIRE( second.v == 1 );
    REQUIRE( apples[second] == 20 );

    u32 seen = 0;
    for( const Apple_id id : apples.ids() )
    {
        REQUIRE( id.v == seen++ );
    }
    REQUIRE( seen == 2 );

    STATIC_REQUIRE( indexable_by<Id_vector<Apple_id, int>, Apple_id> );
    STATIC_REQUIRE_FALSE( indexable_by<Id_vector<Apple_id, int>, Pear_id> );
    STATIC_REQUIRE_FALSE( indexable_by<Id_vector<Apple_id, int>, u32> );
}

} // namespace keel
#endif
