// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include "common/literal_pool.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{
// The bits `value` computes from its operands' bits, nullopt where an operand it needs is unknown or it
// does not fold; wrapped as §12 says, and a `bool` is 0 or 1.
std::optional<u64> evaluate( const Rvalue& value, std::optional<u64> a, std::optional<u64> b, const Type_table& types );

// `bits` of `from` as a `to`: a signed source sign-extends.
std::optional<u64> convert( u64 bits, Type_id from, Type_id to, const Type_table& types );

// `bits` of `type` written as lowering writes a literal: a negative is `-const m`.
Rvalue value_of( u64 bits, Type_id type, const Type_table& types, Literal_pool& literals );

// What `value` reduces to without knowing its operands: a whole local compared with itself, or
// `x + 0`, `x - 0` or `x * 1`; nullopt otherwise.
std::optional<Rvalue> identity( const Rvalue& value, const Type_table& types, Literal_pool& literals );

// `x + c`, `x - c` or `x` for `outer`, `(copy _t) op2 const c2` with `_t = x op1 const c1`, each `op`
// `+` or `-`; nullopt otherwise. The caller knows `outer.a` reads what `inner` wrote, and `x` is unchanged.
std::optional<Rvalue> combined( const Rvalue& outer, const Rvalue& inner, const Type_table& types, Literal_pool& literals );
} // namespace keel