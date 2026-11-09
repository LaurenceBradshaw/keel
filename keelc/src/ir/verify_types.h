// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{

// Checks that a function's statements agree on their types: what verify() cannot, having no table.
class Type_verifier
{
public:
    Type_verifier( const Ast& ast, Types& types, const Interner& interner, std::span<const Function> program );

    std::vector<std::string> check( const Function& func ) const;

    // verify() and check() together; any problem is printed, naming `pass`, and aborts.
    void require( const Function& func, std::string_view pass ) const;

    // check() alone, for KIR as lowered: simplify removes the blocks verify() would call unreachable.
    void require_types( const Function& func, std::string_view pass ) const;

private:
    const Function* callee( const Rvalue& value ) const;
    void            report( const Function& func, std::string_view pass, const std::vector<std::string>& errors ) const;

    const Ast&                                ast_;
    Types&                                    types_;
    const Interner&                           interner_;
    std::span<const Function>                 program_;
    std::unordered_map<u32, std::vector<u32>> by_declaration_; // into program_
};

} // namespace keel
