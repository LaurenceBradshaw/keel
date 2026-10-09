// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ostream>
#include <string>
#include <string_view>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

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

// One identifier: its span covers the name alone, and `declaration` the name where it is declared,
// invalid for a package. `signature` is the declaration as this use sees it, `doc` its `///`, and
// `owner` the declaration a type parameter belongs to, as written.
struct Name
{
    Span        span;
    Name_kind   kind;
    Span        declaration;
    std::string signature = {};
    std::string doc       = {};
    std::string owner     = {};
};

// Every name resolution bound, plus the variants and members named through a type and the fields and
// methods named after a `.`, which only the checker can bind, in source order.
std::vector<Name> collect_names(
    const Ast& ast, const Resolution& resolution, const Types& types, const Source_manager& sm, const Interner& interner
);

// One {"kind":"name",...} line per name, after the diagnostics of --diagnostics=json.
void render_names_json( const Source_manager& sm, const std::vector<Name>& names, std::ostream& out );

} // namespace keel
