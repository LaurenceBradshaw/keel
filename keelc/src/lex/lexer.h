#pragma once

#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/literals.h"
#include "common/source_manager.h"
#include "lex/token.h"

#include <vector>

namespace keel
{

// Lexes a whole file. Errors are reported to diags and lexing continues, so the result is always a
// usable token stream ending in End_of_file; the caller checks diags.has_errors().
std::vector<Token> lex( File_id file, const Source_manager& sm, Interner& interner, Literals& literals, Diagnostics& diags );

} // namespace keel
