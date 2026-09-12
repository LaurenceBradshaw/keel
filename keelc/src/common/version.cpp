#include "common/version.h"

namespace keel
{

std::string_view version_string()
{
    return "keelc 0.1.0 (pre-M0: no front end yet)";
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

TEST_CASE( "version_string_is_populated", "[common]" )
{
    REQUIRE_FALSE( keel::version_string().empty() );
}
#endif // ENABLE_UNIT_TESTS
