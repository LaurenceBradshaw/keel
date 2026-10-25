// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

namespace keel
{

// project()'s VERSION in the root CMakeLists.txt, passed as a define.
inline constexpr std::string_view k_version = KEEL_VERSION;

// "keelc 0.1.0 (<first 12 digits of the build id>)".
std::string version_string();

// SHA-256 of keelc's sources and the prelude, as 64 hex digits.
std::string_view build_id();

} // namespace keel
