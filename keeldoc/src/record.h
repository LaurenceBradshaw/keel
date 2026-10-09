// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace keeldoc
{

// One line of keelc's --diagnostics=json output: a flat object of strings and numbers, each value
// kept as text.
class Record
{
public:
    // Empty when the value is missing.
    std::string_view get( std::string_view key ) const;

    std::map<std::string, std::string, std::less<>> fields;
};

// Nothing when `line` is not a flat JSON object.
std::optional<Record> parse_record( std::string_view line );

} // namespace keeldoc
