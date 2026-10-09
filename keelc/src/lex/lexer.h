// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "lex/token.h"

#include <vector>

namespace keel
{

// Lexes a whole file. Errors are reported to diags and lexing continues, so the result is always a
// usable token stream ending in End_of_file; the caller checks diags.has_errors().
std::vector<Token>
lex( File_id file, const Source_manager& sm, Interner& interner, Literal_pool& literals, Diagnostics& diags );

// D53: the lines `marker` opens in `trivia`, text between two tokens, without the marker and one space.
std::string doc_comment( std::string_view trivia, std::string_view marker );

bool is_identifier( std::string_view text );

} // namespace keel
