// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ir/kir.h"
#include "sema/type_checker.h"

namespace keel
{

// D54: what a statement does to an object a live loan borrows.
enum class Loan_conflict : u8
{
    Moved,    // the object is moved
    Assigned, // the object is assigned or dropped
    Changed   // the object is passed `ref` or `out`, or to a non-`const` method
};

// A loan broken while its holder is still used. Spans only; check/report does the wording.
struct Loan_error
{
    Local_id      object {}; // what is borrowed
    Local_id      holder {}; // the binding that holds the loan
    Span          at {};     // the statement that breaks it
    Span          taken {};  // where the holder was bound
    Loan_conflict conflict = Loan_conflict::Moved;
};

// Per local, whether what it is, or points at, owns anything: only an owner can free or relocate
// what an indirect loan reaches.
std::vector<bool> owning_locals( const Function& func, const Ast& ast, Types& types );

// D54. Each holder is reported once, at its first conflict.
std::vector<Loan_error> check_loans( const Function& func, const std::vector<bool>& owning );

} // namespace keel
