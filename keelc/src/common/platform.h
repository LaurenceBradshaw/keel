// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

namespace keel
{

// What keelc builds for: always its own host, x86-64 either way.
enum class Platform
{
    Linux,
    Windows,
};

#ifdef _WIN32
inline constexpr Platform k_host = Platform::Windows;
#else
inline constexpr Platform k_host = Platform::Linux;
#endif

// As a manifest and a refusal name it.
constexpr std::string_view platform_name( Platform platform )
{
    return platform == Platform::Windows ? "x86_64-windows" : "x86_64-linux";
}

} // namespace keel
