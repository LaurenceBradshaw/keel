#pragma once
#include <string>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{

std::string emit_c_from_kir(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    Types&                       types,
    const Literal_pool&          literals,
    const Source_manager&        sm,
    const Interner&              interner
);

} // namespace keel