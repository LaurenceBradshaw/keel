// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "package.h"

namespace keeldoc
{

// Whether `word` names a type the language builds in, which no declaration documents.
bool is_primitive( std::string_view word );

// primitives.md, compiled into keeldoc: an introduction to the prelude, then one `# name` heading
// per primitive and its doc.
std::string_view primitives_source();

// The prelude page's introduction and one "primitive" entry per heading of `source`, in order.
std::vector<Entry> parse_primitives( std::string_view source, std::string& intro );

} // namespace keeldoc
