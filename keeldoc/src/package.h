// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "record.h"

namespace keeldoc
{

// One {"kind":"declaration",...} record.
struct Declaration
{
    std::string declares;
    std::string name;
    std::string signature;
    std::string doc;
};

// A name's overloads, in source order: most hold one.
struct Group
{
    std::string              name;
    std::vector<Declaration> overloads;

    const std::string& declares() const
    {
        return overloads.front().declares;
    }
};

// A type's members of one kind: "Variants", "Fields", "Constructors" or "Methods".
struct Section
{
    std::string        title;
    std::vector<Group> groups;
};

// A top-level declaration and, for a type, its members.
struct Entry
{
    Group                group;
    std::vector<Section> sections;
};

// A module, named by its path in the package, `a::b` for a/b.kl.
struct Module
{
    std::string        name;
    std::vector<Entry> entries;
};

// From the `// Copyright ...` and `// SPDX-License-Identifier: ...` lines opening a source; empty
// when it has none.
struct Notice
{
    std::string copyright;
    std::string license;
};

// What every program sees without an import: the primitives, then the prelude's declarations.
struct Prelude
{
    std::string        doc;
    std::vector<Entry> entries;
    Notice             notice;
};

struct Package
{
    std::string         name;
    std::string         doc;
    std::vector<Module> modules;
    Notice              notice; // packageinfo.kl's
    Prelude             prelude;
};

// The public declarations of the package `name` in `dir` and of the prelude, from keelc's records,
// with the package's notice and `prelude_source`'s. Each error keelc reported is added to `errors`
// as `file:line:col: error: message`.
Package build_package(
    const std::string&           name,
    const std::filesystem::path& dir,
    const std::vector<Record>&   records,
    std::string_view             prelude_source,
    std::vector<std::string>&    errors
);

} // namespace keeldoc
