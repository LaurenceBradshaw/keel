#pragma once
#include <string>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/literals.h"
#include "common/source_manager.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{

std::string emit_c_from_kir(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    const Types&                 types,
    const Literals&              literals,
    const Source_manager&        sm,
    const Interner&              interner
);

} // namespace keel