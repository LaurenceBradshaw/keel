#pragma once
#include <string>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/literals.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{

// The textual form of a function, for --dump-kir and for golden fixtures.
//
// Names rather than handles throughout - `_1.x` not `_1.<node 37>` - because a Node_id shifts
// whenever anything earlier in the file changes, which would make every golden churn on unrelated
// edits. That is why this needs the Ast and the Interner at all.
std::string
print( const Function& func, const Ast& ast, const Type_table& types, const Literals& literals, const Interner& interner );

} // namespace keel
