// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace keel
{

// The runtime library a program links against: --runtime's path if given, else the installed one.
struct Runtime
{
    std::filesystem::path path;
    std::string           error; // empty when `path` is a file
};

Runtime find_runtime( const std::optional<std::string>& flag, const std::filesystem::path& installed );

// The C compiler's command line, compiling `source` and linking `runtime` into `executable`.
std::string link_command(
    std::string_view             compiler,
    std::string_view             cflags,
    const std::filesystem::path& source,
    const std::filesystem::path& runtime,
    const std::filesystem::path& executable
);

} // namespace keel
