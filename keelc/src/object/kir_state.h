// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "common/bytes.h"
#include "ir/kir.h"
#include "object/refs.h"

// A KIR function in a module's state, as the dataflow checks left it. Every field is walked here,
// so a field added to a KIR struct is added here too.

namespace keel
{

void     write_function( Byte_writer& out, Ref_writer& refs, const Function& function );
Function read_function( Byte_reader& in, Ref_reader& refs );

} // namespace keel
