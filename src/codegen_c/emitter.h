#pragma once
#include <string>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/literals.h"
#include "common/source_manager.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{

// PLAN §7. Turns a checked AST into C11 source text.
//
// Takes no Diagnostics: everything it could object to has already been reported, so it is only
// ever called on a program that type-checked cleanly. If sema reported anything, the driver stops
// before reaching here.
std::string emit_c(
    const Ast&            ast,
    const Resolution&     resolution,
    const Types&          types,
    const Literals&       literals,
    const Source_manager& sm,
    const Interner&       interner
);

} // namespace keel
