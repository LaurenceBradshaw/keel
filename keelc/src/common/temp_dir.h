// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <unistd.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace keel
{

// A directory of its own, named for the process so two keelcs never share one. Removes itself on
// scope exit. Tests write real files in one; the driver compiles its objects in one.
struct Temp_dir
{
    std::filesystem::path path;

    Temp_dir()
    {
        static std::atomic<int> counter { 0 };
        path = std::filesystem::temp_directory_path() /
               ( "keel_" + std::to_string( ::getpid() ) + "_" + std::to_string( counter++ ) );
        std::filesystem::remove_all( path );
        std::filesystem::create_directories( path );
    }

    ~Temp_dir()
    {
        std::error_code ec;
        std::filesystem::remove_all( path, ec );
    }

    std::filesystem::path write( const std::string& name, std::string_view contents ) const
    {
        const std::filesystem::path p = path / name;
        std::filesystem::create_directories( p.parent_path() );
        std::ofstream out( p, std::ios::binary );
        out.write( contents.data(), static_cast<std::streamsize>( contents.size() ) );
        return p;
    }
};

} // namespace keel
