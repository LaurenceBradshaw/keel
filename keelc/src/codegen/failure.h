// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <string>
#include <string_view>
#include "common/source_manager.h"
#include "common/span.h"
#include "ir/kir.h"

namespace keel
{

// What a panic of `failure` prints before the source: "assertion failed: ".
std::string_view failure_prefix( Failure failure );

// The source as written, on one line: each run of whitespace becomes one space.
std::string as_written( const Source_manager& sm, Span span );

} // namespace keel
