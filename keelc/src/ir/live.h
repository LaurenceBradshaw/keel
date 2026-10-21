// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/initialised.h"
#include "ir/kir.h"

namespace keel
{

// Which locals some path still reads, backward over KIR with union at a join. A value nothing reads
// again can share its storage.

// A set per statement, statements × locals bits; per block plus a re-walk if a large function shows it.
struct Liveness
{
    std::vector<Local_set> on_entry; // live at the top of each block, indexed by Block_id::v
    std::vector<Local_set> after;    // live just after each statement, indexed like `func.statements`
};

// What is live at the top of each block and after each statement of `func`.
Liveness live_locals( const Function& func );

// Locals a pointer can read or write at any later statement, out of liveness's sight. `&init` keeps no
// pointer past its constructor.
Local_set addressed_locals( const Function& func );

} // namespace keel