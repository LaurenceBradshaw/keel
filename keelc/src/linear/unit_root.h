// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <complex>
#include "common/types.h"

namespace keel
{
// W_n^k = exp(-2 pi i k / n), each part the correctly rounded true value, so keelc's output never
// rests on the host's libm. n is below 2^53.
std::complex<f64> unit_root( u64 k, u64 n );
} // namespace keel
