#pragma once
#include "ast/ast.h"
#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"

namespace keel
{

// Lexes and parses `input` and every module it imports, transitively, into one tree whose root holds
// every file's declarations. A module is `name.kl` beside the input file, and each is loaded once.
Ast load_program( File_id input, Source_manager& sm, Interner& interner, Literal_pool& literals, Diagnostics& diags );

} // namespace keel
