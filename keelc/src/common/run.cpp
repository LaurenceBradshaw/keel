// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Run is header-only; this file exists only to host its tests. Empty TU in the keelc build.

#ifdef ENABLE_UNIT_TESTS
#include "common/run.h"

#include <catch2/catch_test_macros.hpp>

namespace keel
{

// What a std::span cannot do: the vector reallocates mid-walk.
TEST_CASE( "run_survives_its_vector_growing", "[common][run]" )
{
    std::vector<int> items { 1, 2, 3, 4 };
    items.shrink_to_fit();

    const Run<int> middle( items, 1, 2 );
    int            sum = 0;

    for( const int item : middle )
    {
        sum += item;
        items.resize( items.size() * 4, 0 );
    }

    REQUIRE( sum == 5 );
    REQUIRE( middle[1] == 3 );
    REQUIRE( middle.position( 1 ) == 2 );
}

TEST_CASE( "run_reads_as_read_only", "[common][run]" )
{
    std::vector<int>     items { 7, 8 };
    const Run<int>       writable( items, 0, 2 );
    const Run<const int> read_only = writable;

    writable[0] = 9;
    REQUIRE( read_only[0] == 9 );
    REQUIRE( read_only.size() == 2 );
}

} // namespace keel
#endif
