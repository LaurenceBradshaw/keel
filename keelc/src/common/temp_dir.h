#pragma once
#ifdef ENABLE_UNIT_TESTS
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace keel
{

// A directory of real files for tests that read from disk. Removes itself on scope exit.
struct Temp_dir
{
    std::filesystem::path path;

    Temp_dir()
    {
        static std::atomic<int> counter { 0 };
        path = std::filesystem::temp_directory_path() / ( "keel_test_" + std::to_string( counter++ ) );
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
        std::ofstream               out( p, std::ios::binary );
        out.write( contents.data(), static_cast<std::streamsize>( contents.size() ) );
        return p;
    }
};

} // namespace keel
#endif
