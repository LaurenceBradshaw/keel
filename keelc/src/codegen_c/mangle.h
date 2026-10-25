// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

// Where a declaration lives: its package, empty for the program's own, and its file's stem, empty
// for the input file and the prelude, which are each alone. Two `private` functions of one name in
// two modules are two symbols.
struct Mangled_owner
{
    std::string package = {};
    std::string module  = {};
};

// Each part length-prefixed, so `a_b` with `c` and `a` with `b_c` stay apart; an empty part is
// left out. A name never starts with a digit, so a package with no module reads apart from both.
std::string module_slot( const Mangled_owner& owner );

// `kl_<owner>_<name>__<argtypes>`, plus `__I<typeargs>E__` before them when the function is an
// instantiation. The input file's owner is empty, which gives `kl__add__3i32_3i32`. Parameter types
// are what tell two overloads of one name apart, and the type arguments are what tell two
// instantiations of one generic apart - an overloaded generic needs both.
//
// `enclosing` is the type a member belongs to, and invalid for a free function. It leads the
// argtypes as `S<type>` - inside them rather than beside the name, where a legitimately named free
// function could spell it - and the receiver is then not among the params, since a member's
// receiver can never be what tells two members apart. A method `at( i32 ) const` on `Box` is
// `kl__at__S3Box_3i32`, and the free `at( Box, i32 )` keeps `kl__at__3Box_3i32`. A method written in
// a block also carries the interface it answers, as `C<interface>` after the `S` tag, so it and the
// type's own method of that name stay apart.
std::string mangle_function(
    const Mangled_owner&               owner,
    std::string_view                   name,
    std::span<const Mangled_parameter> params,
    const Type_table&                  types,
    std::span<const Type_id>           type_arguments = {},
    Type_id                            enclosing      = {},
    Type_id                            conformed      = {}
);
// `kl_<owner>_<Name>`, plus `__I<args>E` when the aggregate is an instantiation. The type rather
// than a name, because `Box<i32>` and `Box<f64>` are two C structs from one declaration.
std::string mangle_struct( const Mangled_owner& owner, Type_id type, const Type_table& types );
// `kl__U<members>E`: no package declares a union, and its members name one each.
std::string mangle_union( Type_id type, const Type_table& types );
// `kl__fn__F<params>E<return>`, the name of the C typedef a function type is spelled
// through. A typedef because a C function-pointer type is a declarator, with the name inside it,
// and every caller of Spelling::type writes the type and then the name.
std::string mangle_function_type( Type_id type, const Type_table& types );

// `kl_<owner>_<Type>__dtor`. The suffix sits where argtypes go, so nothing collides with it short
// of a function taking a parameter of a type named `dtor`, which L15's naming rules out. Same
// non-injectivity the scheme already has, and the same fix when it matters: length prefixes.
std::string mangle_destructor(
    const Mangled_owner& owner, std::string_view type_name, std::span<const Type_id> type_arguments, const Type_table& types
);
// Similarly for constructors, which carry both for the same reason: the type arguments say which
// instance, and the parameter types tell that instance's constructors apart.
std::string mangle_constructor(
    const Mangled_owner&               owner,
    std::string_view                   type_name,
    std::span<const Type_id>           type_arguments,
    std::span<const Mangled_parameter> params,
    const Type_table&                  types
);

// A local or parameter. Its Local_id disambiguates: a Keel program may legitimately contain both
// `x` and `kl_x`, which would otherwise become the same C identifier.
std::string mangle_local( std::string_view name, u32 local );

// `kl_<name>`: a struct's fields have distinct names, and the prefix keeps one off a C keyword.
std::string mangle_field( std::string_view name );
// `kl_<owner>_<name>__G` at file scope, `kl_<owner>_<name>__GS<enclosing>` for a static field. `G`
// starts no encoded parameter, so no function's argtypes can spell it.
std::string mangle_global( const Mangled_owner& owner, std::string_view name, Type_id enclosing, const Type_table& types );

std::string mangle_run( Type_id run, const Type_table& types );
std::string mangle_array( Type_id array, const Type_table& types );

} // namespace keel
