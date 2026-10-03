// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include "lex/token.h"

namespace keel
{

// Help for a word dropped from a class body, or empty when there is none.
[[nodiscard]] std::string_view dropped_word_hint( std::string_view word );

// Help for a member head that stopped at `found` where it wanted `wanted`, or empty.
[[nodiscard]] std::string_view member_stop_hint( Token_kind wanted, Token_kind found );

} // namespace keel
