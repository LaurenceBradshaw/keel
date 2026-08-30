#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "common/span.h"
#include "common/types.h"

namespace keel
{

struct Line_col
{
    u32 line; // 1-based
    u32 col;  // 1-based, counted in bytes
};

struct Source_file
{
    std::string      path;
    std::string      text;
    std::vector<u32> line_starts;
};

class Source_manager
{
public:
    // Non-copyable, Non-movable
    Source_manager()                                   = default;
    Source_manager( const Source_manager& )            = delete;
    Source_manager& operator=( const Source_manager& ) = delete;

    File_id                add_file( std::string path, std::string text );
    std::optional<File_id> load_file( const std::filesystem::path& path );

    const Source_file& file( File_id id ) const;
    std::string_view   text( Span s ) const;
    Line_col           line_col( File_id id, u32 offset ) const;
    std::string_view   line_text( File_id id, u32 line ) const;

private:
    static std::vector<u32> build_line_starts( std::string_view text );

    // Append-only: File_id::v is the index. unique_ptr keeps Source_file addresses stable, so
    // string_views into text survive later add_file calls.
    std::vector<std::unique_ptr<Source_file>> files_;
    std::unordered_map<std::string, File_id>  by_path_;
};

} // namespace keel