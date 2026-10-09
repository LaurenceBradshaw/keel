// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "sema/resolver.h"
#include "sema/type_checker.h"

namespace keel
{

// One declaration a page can document: a top-level one, an aggregate's member or an enum's variant.
// `span` covers its name, `kind` is what it declares ("method"), and `parent` names the aggregate or
// enum that holds it, empty at the top level.
struct Declaration
{
    Span             span;
    std::string_view kind;
    Access           access;
    std::string      signature;
    std::string      parent;
    std::string      doc;
};

// Every declaration in the program's files, in source order, each signature as declared.
std::vector<Declaration> collect_declarations(
    const Ast& ast, const Resolution& resolution, const Types& types, const Source_manager& sm, const Interner& interner
);

// One {"kind":"package",...} line per package with a `//!` doc, by name, then one
// {"kind":"declaration",...} line per declaration, naming it as its span spells it ("~list",
// "operator[]"), after the diagnostics of --diagnostics=json.
void render_declarations_json(
    const Source_manager&                       sm,
    const Interner&                             interner,
    const std::unordered_map<u32, std::string>& package_docs,
    const std::vector<Declaration>&             declarations,
    std::ostream&                               out
);

} // namespace keel
