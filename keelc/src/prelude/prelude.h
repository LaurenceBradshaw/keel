// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <string_view>

namespace keel
{
// the path diagnostics name the prelude by; nothing is read from it
inline constexpr std::string_view prelude_path = "<prelude>";
// prelude.kl, compiled into keelc
std::string_view prelude_source();
} // namespace keel