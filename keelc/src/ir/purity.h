// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ast/ast.h"
#include "ir/kir.h"

namespace keel
{
// Which functions are pure: their result depends on their arguments alone. Pure writes only its own
// locals, reads no pointee and no global but a `const` one nothing writes, and calls only pure
// functions; an `extern` never is. Decided a group of `callees_first` at a time.
class Purity
{
public:
    Purity( const std::vector<Function>& functions, const Ast& ast );

    // Decides `group`, once its bodies are final; every group it calls is decided already.
    void decide( const std::vector<u32>& group );

    // A Call of a pure function.
    bool pure( const Rvalue& call ) const;

    // Also certain to return: no loop, no recursion and no panic, here or in what it calls.
    bool removable( const Rvalue& call ) const;

private:
    std::optional<u32> callee( const Rvalue& call ) const;
    bool               pure_body( const Function& func ) const;
    bool               returns( const Function& func ) const;

    const std::vector<Function>&              functions_;
    const Ast&                                ast_;
    std::unordered_map<u32, std::vector<u32>> by_declaration_;
    std::unordered_set<u32>                   written_; // globals some function assigns, drops or lends to write
    std::vector<bool>                         pure_;
    std::vector<bool>                         removable_;
};
} // namespace keel
