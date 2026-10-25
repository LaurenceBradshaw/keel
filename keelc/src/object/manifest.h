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

// A `.klo` as read: its manifest and its state, which the loader reads in place of the module's source.
struct Object
{
    std::string path {};
    u64         hash = 0; // fnv1a of the whole file
    Manifest    manifest {};
    std::string state {};

    const std::string& module() const
    {
        return manifest.module;
    }
};

std::string             write_manifest( const Manifest& manifest );
std::optional<Manifest> read_manifest( std::string_view text );

} // namespace keel
