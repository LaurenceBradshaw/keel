// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include "codegen_c/parts.h"
#include "front_end.h"
#include "object/manifest.h"

// A module compiled with `-c`: a `.klo`, which is an `ar` archive of its manifest (`__.KEEL`), its
// state (`__.KEEL.STATE`), and in a debug build its part's native object.

namespace keel
{

struct Object_options
{
    std::filesystem::path out {}; // the .klo
    std::filesystem::path c {};   // the part's C, kept beside it for its #lines; debug only
    bool                  debug    = false;
    bool                  overflow = false;
    std::string           compiler = "cc";
    std::string           cflags {};

    std::span<const Object> objects {}; // the module's imports, as the front end loaded them
};

// Writes the input module of `front`, which ran with `as_module`, through Stage::Elaborated with
// keep_checked in debug and through Stage::Reported in release. 0, or 1 after saying why on stderr.
int write_object(
    Front_end&            front,
    const Parts&          parts,
    const Literal_pool&   literals,
    const Source_manager& sm,
    const Interner&       interner,
    const Object_options& options
);

// Nothing, with `error` set, when `path` cannot be read or is no Keel object.
std::optional<Object> read_object( const std::filesystem::path& path, std::string& error );

// Why these objects cannot be built with, in command-line order, or nothing when they can.
std::optional<std::string> refused( std::span<const Object> objects, bool debug, bool overflow );

} // namespace keel
