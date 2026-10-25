// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/types.h"

// An object's `__.KEEL` member: what it was built from and against, one fact per line, so
// `ar p x.klo __.KEEL` reads it.

namespace keel
{

struct Manifest
{
    u32         format = 1;
    std::string build {}; // the keelc's build_id()
    bool        debug    = false;
    bool        overflow = false;
    std::string module {}; // qualified, as `kl::time`
    std::string source {};
    u64         source_hash = 0;

    // Each direct import's module and its object's hash, sorted by module.
    std::vector<std::pair<std::string, u64>> depends {};
};

std::string             write_manifest( const Manifest& manifest );
std::optional<Manifest> read_manifest( std::string_view text );

} // namespace keel
