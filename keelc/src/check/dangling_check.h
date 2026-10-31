// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "ir/kir.h"

namespace keel
{

// A read through a pointer whose target has ended. Spans only; check/report does the wording.
struct Dangling_error
{
    Local_id pointer {}; // the pointer read through
    Local_id ended {};   // the local it was given the address of
    Span     at {};      // the reading statement, or the terminator
};

// D54: a read through a pointer after the local it was given the address of has ended, on some
// path. Reported once per pointer.
std::vector<Dangling_error> check_dangling( const Function& func );

} // namespace keel
