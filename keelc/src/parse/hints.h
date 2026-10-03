// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include "lex/token.h"

namespace keel
{

// Where the dropped tokens were: a hint may hold in one place and not the other.
enum class Hint_place : u8
{
    Member,
    Declaration,
};

// Help for a word dropped at `place`, or empty when there is none.
std::string_view dropped_word_hint( std::string_view word, Hint_place place );

// Help for a head at `place` that stopped at `found` where it wanted `wanted`, or empty.
std::string_view stop_hint( Token_kind wanted, Token_kind found, Hint_place place );

} // namespace keel
