// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ostream>
#include <string_view>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "sema/resolver.h"

namespace keel
{

// What a written name refers to, for an editor to colour it by.
enum class Name_kind : u8
{
    Global,
    Variable,
    Parameter,
    Field,
    Function,
    Method,
    Struct,
    Class,
    Enum,
    Variant,
    Type_parameter,
    Package
};

std::string_view name_kind_name( Name_kind kind );

// One identifier: its span covers the name alone.
struct Name
{
    Span      span;
    Name_kind kind;
};

// Every name resolution bound, plus the variants and members named through a type, in source order.
std::vector<Name>
collect_names( const Ast& ast, const Resolution& resolution, const Source_manager& sm, const Interner& interner );

// One {"kind":"name",...} line per name, after the diagnostics of --diagnostics=json.
void render_names_json( const Source_manager& sm, const std::vector<Name>& names, std::ostream& out );

} // namespace keel
