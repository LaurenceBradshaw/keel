#pragma once
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/literals.h"
#include "ir/kir.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{

// A Function per Function_decl, in declaration order.
std::vector<Function>
// Types is mutable because an instantiation substitutes its parameters away, and `T*` becoming
// `i32*` interns a type that may not exist yet.
lower( const Ast& ast, const Resolution& resolution, Types& types, Literals& literals, const Interner& interner );

} // namespace keel