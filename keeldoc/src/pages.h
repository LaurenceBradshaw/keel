// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <map>
#include <string>
#include "package.h"

namespace keeldoc
{

// Every file of the package's pages by name: index.html, one page per module and style.css. The
// same package always gives the same bytes.
std::map<std::string, std::string> render_pages( const Package& package );

// A module's page, `a::b` at a.b.html.
std::string page_of( const Module& module );

} // namespace keeldoc
