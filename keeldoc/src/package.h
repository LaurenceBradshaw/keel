// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <filesystem>
#include <map>
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
    std::string section {};
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

// A type's members of one kind, "Variants", "Fields", "Constructors" or "Methods", or under one
// banner, after the kinds.
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

// The nested package `module` is in, `a` for a::b; empty for the package's own.
inline std::string_view nested_package_of( const Module& module )
{
    const std::size_t last = module.name.rfind( "::" );
    return last == std::string::npos ? std::string_view {} : std::string_view( module.name ).substr( 0, last );
}

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
    std::string                        name;
    std::string                        doc;
    std::vector<Module>                modules; // the package's own, then each nested package's
    Notice                             notice;  // packageinfo.kl's
    Prelude                            prelude;
    std::map<std::string, std::string> nested_docs {}; // by the nested package's name in this one, `a` for geo::a
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
