// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

namespace keeldoc
{

// A doc comment's Markdown as HTML. The subset: paragraphs, `#` headings of any level as one
// section heading, `-` `*` `+` and `1.` lists, fenced code, and inline code, `*em*`, `**strong**`,
// `[text](url)` and backslash escapes. Anything else is text.
std::string render_markdown( std::string_view text );

// One line of Markdown's inline syntax as HTML.
std::string render_inline( std::string_view text );

// The first paragraph, the doc's summary, as inline HTML.
std::string render_summary( std::string_view text );

} // namespace keeldoc
