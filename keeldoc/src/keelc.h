// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "record.h"

namespace keeldoc
{

// Each module of the package in `dir`: its .kl files but packageinfo.kl, as `name::module`, sorted.
std::vector<std::string> package_modules( const std::string& name, const std::filesystem::path& dir );

// keelc's records for a program importing every module of the package `name` in `dir`. Empty, with
// `error` set, when keelc could not run.
std::vector<Record>
run_keelc( const std::filesystem::path& keelc, const std::string& name, const std::filesystem::path& dir, std::string& error );

} // namespace keeldoc
