// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <filesystem>
#include <span>
#include <string>

namespace keel
{

// Links `objects`, then `runtime`, then `linked` (the user's objects, archives, -L and -l words) and
// libc into `executable`, with lld in-process. Empty on success, else what lld reported.
std::string link_executable(
    std::span<const std::filesystem::path> objects,
    const std::filesystem::path&           runtime,
    const std::filesystem::path&           executable,
    std::span<const std::string>           linked
);

} // namespace keel
