// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <string>
#include <vector>
#include "ir/kir.h"

namespace keel
{

std::vector<std::string> verify( const Function& func );

} // namespace keel