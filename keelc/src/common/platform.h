// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <cstdlib>
#include <filesystem>
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

// keelc's own cache: %LOCALAPPDATA%\keel, or $XDG_CACHE_HOME/keel, then ~/.cache/keel; empty when
// none of those is set.
inline std::filesystem::path cache_folder()
{
    const auto set = []( const char* name ) -> std::filesystem::path
    {
        const char* const value = std::getenv( name );
        return value != nullptr && *value != '\0' ? std::filesystem::path( value ) : std::filesystem::path {};
    };
    if constexpr( k_host == Platform::Windows )
    {
        const std::filesystem::path local = set( "LOCALAPPDATA" );
        return local.empty() ? local : local / "keel";
    }
    if( const std::filesystem::path xdg = set( "XDG_CACHE_HOME" ); !xdg.empty() )
    {
        return xdg / "keel";
    }
    const std::filesystem::path home = set( "HOME" );
    return home.empty() ? home : home / ".cache" / "keel";
}

} // namespace keel
