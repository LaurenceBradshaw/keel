// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>
#include "ast/ast.h"
#include "common/diagnostics.h"
#include "common/interner.h"
#include "ir/kir.h"
#include "ir/purity.h"
#include "linear/recognise.h"
#include "linear/sums.h"
#include "parse/module_graph.h"
#include "sema/type_checker.h"

namespace keel
{
// The `@dft` functions a plan may call, what each instance's planned body costs, and the check of
// each promise against the map its body computes.
class Promises
{
public:
    // An instance lowered and optimised; its index.
    using Instantiate = std::function<u32( Instantiation )>;
    // Function `f` optimised, and first what it calls; whether it is, as one under way is not.
    using Want = std::function<bool( u32 )>;

    Promises(
        const Ast&                   ast,
        Types&                       types,
        const Module_graph&          modules,
        const std::vector<Function>& functions,
        Instantiate                  instantiate,
        Want                         want
    );

    // What a plan in `from` may call at each n: the `@dft` its module finds, a fixed one over a
    // generic, which is instantiated at n. Inside a `@dft` body at N, none at N; one still being
    // optimised has no cost yet, which ends a cycle.
    std::function<std::optional<Promised>( u64 )> for_plan( const Function& from );

    // What `f`'s body costs once the linear map pass has read it.
    void planned( u32 f, std::optional<Cost> cost );

    // Each `@dft` declared outside a loaded module, a generic one at N = 2, 4 and 8, a fixed one at
    // its own N: an error at the directive where the body computes another map.
    void check(
        const Purity&           purity,
        const Literal_pool&     literals,
        const Global_constants& globals,
        const Interner&         interner,
        Diagnostics&            diags
    );

private:
    u64                     size( Node_id dft ) const;
    std::optional<u32>      instance( File_id from, u64 n );
    std::optional<Promised> promised( File_id from, u64 n );

    const Ast&                    ast_;
    Types&                        types_;
    const Module_graph&           modules_;
    const std::vector<Function>&  functions_;
    Instantiate                   instantiate_;
    Want                          want_;
    std::vector<Node_id>          dfts_;
    std::unordered_map<u32, Cost> costs_;
};
} // namespace keel
