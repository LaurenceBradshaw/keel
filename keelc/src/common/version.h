// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

namespace keel
{

// Bumped by hand. Kept out of CMake's configure_file machinery deliberately: a generated header
// would break editor IntelliSense on a clean checkout, before the first configure has run.
inline constexpr std::string_view k_version = "0.1.0";

// "keelc 0.1.0 (<first 12 digits of the build id>)".
std::string version_string();

// SHA-256 of keelc's sources and the prelude, as 64 hex digits.
std::string_view build_id();

} // namespace keel
