#pragma once

#include "ast/ast.h"
#include "common/diagnostics.h"
#include "common/source_manager.h"
#include "lex/token.h"

#include <span>

namespace keel
{

// Builds the tree for a whole token stream. Errors are reported to diags and parsing continues, so
// the result is always a usable tree; the caller checks diags.has_errors().
//
// No Interner: the lexer already interned every identifier into Token::symbol, and keyword checks
// compare against compile-time Keyword constants.
Ast parse( std::span<const Token> tokens, const Source_manager& sm, Diagnostics& diags );

} // namespace keel
