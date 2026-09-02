#pragma once
#include <span>
#include <string>
#include <string_view>
#include "common/types.h"
#include "sema/type.h"

namespace keel
{

// PLAN §7.5. Every emitted symbol carries the `kl_` prefix so a user identifier can never collide
// with a C keyword, a libc name, or the runtime. Mangling is spelled in *Keel* type names, not C
// ones - `kl__add__i32_i32` - so this file knows nothing about the C backend.

// `kl_<module>_<name>__<argtypes>`. The module is empty until M7, which gives `kl__add__i32_i32`.
// Parameter types are what make overloads distinct, so they are part of the name even in v0 where
// no overloads exist.
std::string
mangle_function( std::string_view module, std::string_view name, std::span<const Type_id> params, const Type_table& types );

// A local or parameter. The declaration's node id disambiguates: a Keel program may legitimately
// contain both `x` and `kl_x`, which would otherwise become the same C identifier.
std::string mangle_local( std::string_view name, u32 declaration );

} // namespace keel
