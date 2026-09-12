#pragma once

#include <string_view>

namespace keel
{

// Bumped by hand. Kept out of CMake's configure_file machinery deliberately: a generated header
// would break editor IntelliSense on a clean checkout, before the first configure has run.
inline constexpr std::string_view k_version = "0.1.0";

// "keelc 0.1.0 (<milestone>)" - the milestone tag is the honest signal of what the compiler can
// actually do, and is more useful than the version number during early development.
std::string_view version_string();

} // namespace keel
