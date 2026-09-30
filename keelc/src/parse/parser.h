#pragma once

#include <span>
#include <vector>
#include "ast/ast.h"
#include "common/diagnostics.h"
#include "common/source_manager.h"
#include "lex/token.h"

namespace keel
{

// Builds the tree for a whole token stream. Errors are reported to diags and parsing continues, so
// the result is always a usable tree; the caller checks diags.has_errors().
//
// No Interner: the lexer already interned every identifier into Token::symbol, and keyword checks
// compare against compile-time Keyword constants.
Ast parse( std::span<const Token> tokens, const Source_manager& sm, Diagnostics& diags );

// Parses one file's declarations into `ast` and returns them, without building a root, so the loader can put several files
// under one.
std::vector<Node_id> parse_into( Ast& ast, std::span<const Token> tokens, const Source_manager& sm, Diagnostics& diags );

} // namespace keel
