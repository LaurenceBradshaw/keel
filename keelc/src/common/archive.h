// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace keel
{

struct Archive_member
{
    std::string              name;
    std::string              data;
    std::vector<std::string> symbols; // what the member defines, for the index
};

// A System V `ar` archive, as GNU ld, gold and lld read it: the symbol index `/`, the long-name
// table `//` when a name passes 15 bytes, then each member. Every mtime, uid and gid is zero, so
// the bytes depend only on the members.
std::string write_archive( std::span<const Archive_member> members );

// The members of an archive `write_archive` wrote, or nothing if the bytes are not one. The index is
// skipped, so each member's `symbols` is empty.
std::optional<std::vector<Archive_member>> read_archive( std::string_view bytes );

} // namespace keel
