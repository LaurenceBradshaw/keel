// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ast/ast.h"
#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{

// The wording for what the dataflow checks find: use after move (§8), D54's broken loans, and D9's
// and D31's unassigned reads, `out` parameters and return slot. The passes return spans and locals;
// this names them.
void report_dataflow_errors(
    const std::vector<Function>& functions,
    const Ast&                   ast,
    const Source_manager&        sm,
    const Interner&              interner,
    Types&                       types,
    Diagnostics&                 diagnostics
);

} // namespace keel
