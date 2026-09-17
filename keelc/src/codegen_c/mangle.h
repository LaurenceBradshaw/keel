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
// `kl_<module>_<name>__<argtypes>`, plus `__<typeargs>` when the function is an instantiation.
// Two instantiations of one generic share a declaration and every value-parameter type once
// substituted, so the type arguments are the only thing that can tell them apart.
std::string mangle_function(
    std::string_view         module,
    std::string_view         name,
    std::span<const Type_id> params,
    const Type_table&        types,
    std::span<const Type_id> type_arguments = {}
);
// `kl_<module>_<Name>`, plus `__I<args>E` when the aggregate is an instantiation. The type rather
// than a name, because `Box<i32>` and `Box<f64>` are two C structs from one declaration.
std::string mangle_struct( std::string_view module, Type_id type, const Type_table& types );
// `kl_<module>_<Type>__dtor`. The suffix sits where argtypes go, so nothing collides with it short
// of a function taking a parameter of a type named `dtor`, which L15's naming rules out. Same
// non-injectivity the scheme already has, and the same fix when it matters: length prefixes.
std::string mangle_destructor(
    std::string_view module, std::string_view type_name, std::span<const Type_id> type_arguments, const Type_table& types
);
// Similarly for constructors, yet they do have arg types because they are overloadable.
std::string mangle_constructor(
    std::string_view         module,
    std::string_view         type_name,
    std::span<const Type_id> type_arguments,
    std::span<const Type_id> params,
    const Type_table&        types
);

// A local or parameter. The declaration's node id disambiguates: a Keel program may legitimately
// contain both `x` and `kl_x`, which would otherwise become the same C identifier.
std::string mangle_local( std::string_view name, u32 declaration );

} // namespace keel
