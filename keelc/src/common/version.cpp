// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/version.h"
#include <fmt/format.h>

namespace keel
{

std::string version_string()
{
    return fmt::format( "keelc {} ({})", k_version, build_id().substr( 0, 12 ) );
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

TEST_CASE( "version_string_is_populated", "[common]" )
{
    REQUIRE_FALSE( keel::version_string().empty() );
}

TEST_CASE( "build_id_is_64_hex_digits", "[common]" )
{
    const std::string_view id = keel::build_id();

    REQUIRE( id.size() == 64 );
    REQUIRE( id.find_first_not_of( "0123456789abcdef" ) == std::string_view::npos );
}
#endif // ENABLE_UNIT_TESTS
