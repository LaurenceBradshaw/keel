// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ir/kir.h"

namespace keel
{
// In -O3's loop phase: a read of a place through a pointer, where every path to it copied the place to a
// local that still holds it, reads that local; a call, a drop, or a store through a pointer or to an
// addressed local between them keeps the read. Whether it changed anything.
bool forward_reads( Function& func );
} // namespace keel
