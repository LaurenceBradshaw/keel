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

// One parameter as the mangler sees it: its declared type, and the marker a call site writes for
// it. The marker is part of the name because it is part of what tells two overloads apart - a
// `ref i32` and an `i32*` are different parameters that would otherwise encode alike, and so are a
// `move i32` and a bare one. A `const ref i32` takes no marker, which is precisely why it may not
// be declared beside a bare `i32`, so the two sharing an encoding costs nothing.
//
// They are not the whole of it: a member and a free function of one name match on every parameter
// and every marker, and mangle_function's `enclosing` is what separates those.
struct Mangled_parameter
{
    Type_id type;
    char    marker = '\0'; // 'R' to borrow, 'M' to transfer, 'O' to assign, none for a plain value
};

// `kl_<module>_<name>__<argtypes>`, plus `__I<typeargs>E__` before them when the function is an
// instantiation. The module is empty until M8, which gives `kl__add__3i32_3i32`. Parameter types
// are what tell two overloads of one name apart, and the type arguments are what tell two
// instantiations of one generic apart - an overloaded generic needs both.
//
// `enclosing` is the type a member belongs to, and invalid for a free function. It leads the
// argtypes as `S<type>` - inside them rather than beside the name, where a legitimately named free
// function could spell it - and the receiver is then not among the params, since a member's
// receiver can never be what tells two members apart. A method `at( i32 ) const` on `Box` is
// `kl__at__S3Box_3i32`, and the free `at( Box, i32 )` keeps `kl__at__3Box_3i32`.
std::string mangle_function(
    std::string_view                   module,
    std::string_view                   name,
    std::span<const Mangled_parameter> params,
    const Type_table&                  types,
    std::span<const Type_id>           type_arguments = {},
    Type_id                            enclosing      = {}
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
// Similarly for constructors, which carry both for the same reason: the type arguments say which
// instance, and the parameter types tell that instance's constructors apart.
std::string mangle_constructor(
    std::string_view                   module,
    std::string_view                   type_name,
    std::span<const Type_id>           type_arguments,
    std::span<const Mangled_parameter> params,
    const Type_table&                  types
);

// A local or parameter. The declaration's node id disambiguates: a Keel program may legitimately
// contain both `x` and `kl_x`, which would otherwise become the same C identifier.
std::string mangle_local( std::string_view name, u32 declaration );

} // namespace keel
