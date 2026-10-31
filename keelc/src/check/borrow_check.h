// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"
#include "sema/type_checker.h"

namespace keel
{

// D54: what a statement does to an object a live loan borrows.
enum class Loan_conflict : u8
{
    Moved,    // the object is moved
    Assigned, // the object is assigned or dropped
    Changed,  // the object is passed `ref` or `out`, or to a non-`const` method
    Replaced  // what an element of the object owns is freed or relocated, through a call's result
};

// A loan broken while its holder is still used. Spans only; check/report does the wording.
struct Loan_error
{
    Local_id      object {};   // what is borrowed
    Place         reached {};  // `Changed` or `Moved`: the place passed, called on or moved, when it is in `object`
    Local_id      holder {};   // the binding that holds the loan
    Place         borrowed {}; // what the loan is of, when it is in `object`
    Span          at {};       // the statement that breaks it
    Span          taken {};    // where the holder was bound
    Loan_conflict conflict = Loan_conflict::Moved;
    bool          walked   = false; // the holder is a walk's receiver (D74), and has no name
};

// Per local, whether what it is, or points at, owns anything: only an owner can free or relocate
// what an indirect loan reaches.
std::vector<bool> owning_locals( const Function& func, const Ast& ast, Types& types );

// Per local, whether its value can hold an address, which a call may have returned into its arguments.
std::vector<bool> addressing_locals( const Function& func, const Ast& ast, Types& types );

// D54. Each holder is reported once, at its first conflict. `owning` and `addressing` are the two
// functions above.
std::vector<Loan_error> check_loans(
    const Function& func, const std::vector<bool>& owning, const std::vector<bool>& addressing, const Literal_pool& literals
);

} // namespace keel
