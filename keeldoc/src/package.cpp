// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "package.h"
#include <fmt/format.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include "primitives.h"

namespace keeldoc
{
namespace
{

constexpr std::string_view k_sections[][2] = {
    { "variant", "Variants" },
    { "field", "Fields" },
    { "static_field", "Fields" },
    { "constructor", "Constructors" },
    { "destructor", "Constructors" },
    { "method", "Methods" },
};

// keelc's name for the prelude's source, which is no file on disk.
constexpr std::string_view k_prelude_file = "<prelude>";

constexpr std::string_view k_section_order[] = { "Variants", "Fields", "Constructors", "Methods" };

std::string_view section_of( std::string_view declares )
{
    for( const auto& [kind, title] : k_sections )
    {
        if( kind == declares )
        {
            return title;
        }
    }

    return "Methods";
}

// `file`'s module in the package at `dir`, or nothing when it is outside it.
std::optional<std::string> module_of( const std::filesystem::path& dir, std::string_view file )
{
    std::error_code             ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical( dir, ec );
    const std::filesystem::path path = std::filesystem::weakly_canonical( std::filesystem::path( file ), ec );
    if( ec || path.extension() != ".kl" )
    {
        return std::nullopt;
    }

    const std::filesystem::path relative = path.lexically_relative( root );
    if( relative.empty() || *relative.begin() == ".." )
    {
        return std::nullopt;
    }

    std::string name;
    for( const std::filesystem::path& part : relative.parent_path() / relative.stem() )
    {
        name += ( name.empty() ? "" : "::" ) + part.string();
    }

    return name;
}

// Adds `decl` to the group of its name in `groups`, keeping the order each name first appears.
void add_overload( std::vector<Group>& groups, Declaration decl )
{
    const auto same = std::ranges::find_if(
        groups, [&]( const Group& group ) { return group.name == decl.name && group.declares() == decl.declares; }
    );

    if( same != groups.end() )
    {
        same->overloads.push_back( std::move( decl ) );
        return;
    }

    std::string name = decl.name;
    groups.push_back( { std::move( name ), { std::move( decl ) } } );
}

// The copyright and licence from the plain comments opening `in`.
Notice read_notice( std::istream& in )
{
    Notice notice;

    for( std::string line; std::getline( in, line ) && line.starts_with( "// " ); )
    {
        const std::string_view text = std::string_view( line ).substr( 3 );

        if( text.starts_with( "Copyright " ) )
        {
            notice.copyright = text;
        }
        else if( text.starts_with( "SPDX-License-Identifier: " ) )
        {
            notice.license = text.substr( std::string_view( "SPDX-License-Identifier: " ).size() );
        }
    }

    return notice;
}

} // namespace

Package build_package(
    const std::string&           name,
    const std::filesystem::path& dir,
    const std::vector<Record>&   records,
    std::string_view             prelude_source,
    std::vector<std::string>&    errors
)
{
    std::ifstream      packageinfo( dir / "packageinfo.kl" );
    std::istringstream prelude( std::string { prelude_source } );

    Package package { name, {}, {}, read_notice( packageinfo ), { {}, {}, read_notice( prelude ) } };
    package.prelude.entries = parse_primitives( primitives_source(), package.prelude.doc );

    std::map<std::string, std::vector<Group>>                                                     top_level;
    std::map<std::pair<std::string, std::string>, std::map<std::string_view, std::vector<Group>>> members;
    std::set<std::pair<std::string, std::string>>                                                 hidden;

    for( const Record& record : records )
    {
        const std::string_view kind = record.get( "kind" );

        if( kind == "diagnostic" && record.get( "severity" ) == "error" )
        {
            errors.push_back( fmt::format(
                "{}:{}:{}: error: {}", record.get( "file" ), record.get( "line" ), record.get( "col" ), record.get( "message" )
            ) );
            continue;
        }

        if( kind == "package" && record.get( "name" ) == name )
        {
            package.doc = record.get( "doc" );
            continue;
        }

        if( kind != "declaration" )
        {
            continue;
        }

        const std::optional<std::string> module =
            record.get( "file" ) == k_prelude_file ? std::string( k_prelude_file ) : module_of( dir, record.get( "file" ) );
        if( !module )
        {
            continue;
        }

        const std::string parent( record.get( "parent" ) );
        Declaration       decl {
            std::string( record.get( "declares" ) ),
            std::string( record.get( "name" ) ),
            std::string( record.get( "signature" ) ),
            std::string( record.get( "doc" ) ),
        };

        // An undocumented extern is C the reader should not call or is documented elsewhere.
        if( decl.signature.starts_with( "extern " ) && decl.doc.empty() )
        {
            continue;
        }

        // A private type's members go with it, whatever their own access.
        if( record.get( "access" ) == "private" || hidden.contains( { *module, parent } ) )
        {
            if( parent.empty() )
            {
                hidden.insert( { *module, decl.name } );
            }
            continue;
        }

        if( parent.empty() )
        {
            add_overload( top_level[*module], std::move( decl ) );
        }
        else
        {
            const std::string_view section = section_of( decl.declares );
            add_overload( members[{ *module, parent }][section], std::move( decl ) );
        }
    }

    for( auto& [module_name, groups] : top_level )
    {
        Module module { module_name, {} };

        for( Group& group : groups )
        {
            Entry entry { std::move( group ), {} };

            if( const auto it = members.find( { module_name, entry.group.name } ); it != members.end() )
            {
                for( const std::string_view title : k_section_order )
                {
                    if( const auto section = it->second.find( title ); section != it->second.end() )
                    {
                        entry.sections.push_back( { std::string( title ), std::move( section->second ) } );
                    }
                }
            }

            module.entries.push_back( std::move( entry ) );
        }

        if( module_name == k_prelude_file )
        {
            std::ranges::move( module.entries, std::back_inserter( package.prelude.entries ) );
            continue;
        }

        package.modules.push_back( std::move( module ) );
    }

    return package;
}

} // namespace keeldoc

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

namespace keeldoc
{
namespace
{

std::vector<Record> parse_all( std::initializer_list<std::string_view> lines )
{
    std::vector<Record> records;
    for( const std::string_view line : lines )
    {
        records.push_back( *parse_record( line ) );
    }

    return records;
}

TEST_CASE( "package_groups_overloads_and_sections_and_drops_private_and_bare_externs", "[package]" )
{
    // clang-format off
    const std::vector<Record> records = parse_all( {
        R"json({"kind":"file","path":"/tmp/root.kl"})json",
        R"json({"kind":"package","name":"geo","doc":"Plane geometry."})json",
        R"json({"kind":"package","name":"other","doc":"Not this one."})json",
        R"json({"kind":"declaration","declares":"class","name":"Box","file":"geo/box.kl","access":"public","signature":"class Box"})json",
        R"json({"kind":"declaration","declares":"method","name":"get","file":"geo/box.kl","access":"public","signature":"public i32 get()","parent":"Box"})json",
        R"json({"kind":"declaration","declares":"constructor","name":"Box","file":"geo/box.kl","access":"public","signature":"public Box()","parent":"Box","doc":"Empty."})json",
        R"json({"kind":"declaration","declares":"field","name":"n","file":"geo/box.kl","access":"private","signature":"private i32 n","parent":"Box"})json",
        R"json({"kind":"declaration","declares":"constructor","name":"Box","file":"geo/box.kl","access":"public","signature":"public Box( i32 n )","parent":"Box"})json",
        R"json({"kind":"declaration","declares":"function","name":"area","file":"geo/box.kl","access":"public","signature":"f64 area( Box b )"})json",
        R"json({"kind":"declaration","declares":"struct","name":"Secret","file":"geo/box.kl","access":"private","signature":"struct Secret"})json",
        R"json({"kind":"declaration","declares":"field","name":"x","file":"geo/box.kl","access":"public","signature":"public i32 x","parent":"Secret"})json",
        R"json({"kind":"declaration","declares":"function","name":"area","file":"geo/box.kl","access":"public","signature":"f64 area( i32 side )"})json",
        R"json({"kind":"declaration","declares":"function","name":"elsewhere","file":"lib/x.kl","access":"public","signature":"void elsewhere()"})json",
        R"json({"kind":"declaration","declares":"function","name":"c_only","file":"geo/box.kl","access":"public","signature":"extern void c_only()"})json",
        R"json({"kind":"declaration","declares":"function","name":"c_doc","file":"geo/box.kl","access":"public","signature":"extern void c_doc()","doc":"Ours."})json",
        R"json({"kind":"declaration","declares":"class","name":"str","file":"<prelude>","access":"public","signature":"class str"})json",
        R"json({"kind":"declaration","declares":"constructor","name":"str","file":"<prelude>","access":"private","signature":"private str()","parent":"str"})json",
    } );
    // clang-format on

    std::vector<std::string> errors;
    const Package            package = build_package( "geo", "geo", records, "", errors );

    REQUIRE( errors.empty() );
    REQUIRE( package.doc == "Plane geometry." );
    REQUIRE( package.modules.size() == 1 );

    const Module& box = package.modules[0];
    REQUIRE( box.name == "box" );
    REQUIRE( box.entries.size() == 3 );
    REQUIRE( box.entries[0].group.name == "Box" );
    REQUIRE( box.entries[1].group.overloads.size() == 2 );
    REQUIRE( box.entries[2].group.name == "c_doc" );

    const std::vector<Section>& sections = box.entries[0].sections;
    REQUIRE( sections.size() == 2 );
    REQUIRE( sections[0].title == "Constructors" );
    REQUIRE( sections[0].groups.size() == 1 );
    REQUIRE( sections[0].groups[0].overloads.size() == 2 );
    REQUIRE( sections[0].groups[0].overloads[0].doc == "Empty." );
    REQUIRE( sections[1].title == "Methods" );

    // The primitives, then the prelude's own declarations, never a module of the package.
    const std::vector<Entry>& prelude = package.prelude.entries;
    REQUIRE( prelude.front().group.declares() == "primitive" );
    REQUIRE( prelude.back().group.name == "str" );
    REQUIRE( prelude.back().sections.empty() );
}

TEST_CASE( "package_reads_its_notice_and_the_prelude's", "[package]" )
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "keeldoc_test_notice";
    std::filesystem::remove_all( dir );
    std::filesystem::create_directories( dir );
    std::ofstream( dir / "packageinfo.kl" ) << "// Copyright 2026 A. Author\n"
                                               "// SPDX-License-Identifier: MIT\n"
                                               "\n"
                                               "//! The package.\n"
                                               "// Copyright 1999 Not this one\n";

    std::vector<std::string> errors;
    const Package            package = build_package( "geo", dir, {}, "// Copyright 2025 B\n", errors );

    REQUIRE( package.notice.copyright == "Copyright 2026 A. Author" );
    REQUIRE( package.notice.license == "MIT" );
    REQUIRE( package.prelude.notice.copyright == "Copyright 2025 B" );
    REQUIRE( package.prelude.notice.license.empty() );

    std::filesystem::remove_all( dir );
    REQUIRE( build_package( "geo", dir, {}, "", errors ).notice.copyright.empty() );
}

TEST_CASE( "package_reports_keelc_errors", "[package]" )
{
    const std::vector<Record> records = parse_all( {
        R"json({"kind":"diagnostic","severity":"warning","file":"geo/a.kl","line":1,"col":2,"message":"unused"})json",
        R"json({"kind":"diagnostic","severity":"error","file":"geo/a.kl","line":3,"col":4,"message":"no such name"})json",
    } );

    std::vector<std::string> errors;
    build_package( "geo", "geo", records, "", errors );

    REQUIRE( errors == std::vector<std::string> { "geo/a.kl:3:4: error: no such name" } );
}

} // namespace
} // namespace keeldoc
#endif // ENABLE_UNIT_TESTS
