// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/devirtualise_calls.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include "ir/place_type.h"

namespace keel
{
namespace
{
bool is_receiver_call( const Ast& ast, const Rvalue& value )
{
    return value.kind == Rvalue_kind::Call && ast.is_requirement( value.callee ) && !value.arguments.empty() &&
           value.arguments[0].kind == Operand_kind::Copy && value.arguments[0].place.is_whole_local();
}

// Where a pointer was made: `&x`, with `x` reached only through fields, or `copy q` with `q` a
// pointer at `type`.
struct Origin
{
    Rvalue  made;
    Type_id type;
};

// Whether `place` is the local or a part of it, not something it points at.
bool in_local( const Function& func, const Place& place )
{
    return !place.is_global() &&
           ( place.num_projections == 0 || func.projections_of( place )[0].kind != Projection_kind::Deref );
}

// Each local's one whole assignment, or null where it has none, more, or is addressed; and which
// keep one value throughout: those, and parameters never written or addressed.
struct Assignments
{
    std::vector<const Statement*> only;
    std::vector<bool>             stable;
};

Assignments single_assignments( const Function& func )
{
    Assignments       result { std::vector<const Statement*>( func.locals.size(), nullptr ), {} };
    std::vector<bool> refused( func.locals.size(), false );

    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign )
        {
            continue;
        }

        const Rvalue& value = func.rvalue( statement );
        if( value.kind == Rvalue_kind::Address_of && in_local( func, value.a.place ) )
        {
            refused[value.a.place.local.v] = true;
        }

        if( in_local( func, statement.place ) )
        {
            const u32 p    = statement.place.local.v;
            refused[p]     = refused[p] || result.only[p] != nullptr || !statement.place.is_whole_local();
            result.only[p] = &statement;
        }
    }

    for( u32 p = 0; p < result.only.size(); ++p )
    {
        result.only[p] = refused[p] ? nullptr : result.only[p];
        result.stable.push_back( !refused[p] && ( result.only[p] != nullptr || ( p > 0 && p <= func.parameter_count ) ) );
    }

    return result;
}

// Back through copies and `&(*q)` to the concrete pointer `local` holds, if any.
std::optional<Origin>
origin_of( const Ast& ast, Types& types, const Function& func, const Assignments& assigned, Local_id local )
{
    const Type_table& table = types.table();

    // `copy q`, where `q` is a concrete pointer that keeps its value, so is still the same at the call.
    const auto concrete = [&]( Local_id q ) -> std::optional<Origin>
    {
        const Type_id type = func.locals[q].type;
        if( !assigned.stable[q.v] || !table.is_pointer( type ) || table.is_interface( table.get( type ).element ) )
        {
            return std::nullopt;
        }
        return Origin {
            { .kind = Rvalue_kind::Use, .type = type, .a = copy( Place { .local = q }, type ) }, table.get( type ).element
        };
    };

    for( u32 hops = 0; hops < func.locals.size(); ++hops )
    {
        if( assigned.only[local.v] == nullptr )
        {
            return std::nullopt;
        }

        const Rvalue& value       = func.rvalue( *assigned.only[local.v] );
        const auto    projections = func.projections_of( value.a.place );
        const bool    copied      = value.a.kind == Operand_kind::Copy && value.a.place.is_whole_local();
        const bool    through =
            !value.a.place.is_global() && projections.size() == 1 && projections[0].kind == Projection_kind::Deref;

        if( value.kind == Rvalue_kind::Address_of && !value.a.place.is_global() && !through &&
            std::ranges::all_of( projections, []( const Projection& p ) { return p.kind == Projection_kind::Field; } ) )
        {
            const Type_id type = place_type( ast, types, func, value.a.place );
            return table.is_interface( type ) ? std::nullopt : std::optional<Origin> { { value, type } };
        }

        if( !( value.kind == Rvalue_kind::Address_of && through ) &&
            ( ( value.kind != Rvalue_kind::Use && value.kind != Rvalue_kind::Cast ) || !copied ) )
        {
            return std::nullopt;
        }

        local = value.a.place.local;
        if( std::optional<Origin> origin = concrete( local ) )
        {
            return origin;
        }
    }

    return std::nullopt;
}
} // namespace

bool devirtualise_calls( Function& func, const Ast& ast, Types& types )
{
    Type_table&                          table    = types.table();
    const u32                            count    = narrow_cast<u32>( func.locals.size() );
    const Assignments                    assigned = single_assignments( func );
    const std::vector<const Statement*>& only     = assigned.only;
    std::vector<std::vector<Statement*>> calls( count );

    for( Statement& statement : func.statements )
    {
        const Rvalue& value = std::as_const( func ).rvalue( statement );
        if( statement.kind == Statement_kind::Assign && is_receiver_call( ast, value ) )
        {
            calls[value.arguments[0].place.local.v].push_back( &statement );
        }
    }

    // Any mention but its own assignment, a storage marker or a receiver keeps the pointer fat.
    std::vector<u32> uses( count, 0 );
    for_each_place(
        std::as_const( func ),
        [&]( const Place& place, const Statement* owner )
        {
            if( place.is_global() || !place.local.is_valid() )
            {
                return;
            }

            const bool marker = owner != nullptr &&
                                ( owner->kind == Statement_kind::Storage_live || owner->kind == Statement_kind::Storage_dead );
            const bool target = owner != nullptr && owner == only[place.local.v];
            uses[place.local.v] += !marker && !target;
        }
    );

    bool changed = false;

    for( u32 p = 0; p < count; ++p )
    {
        const Type_id held = func.locals[Local_id { p }].type;

        if( calls[p].empty() || uses[p] != calls[p].size() || !table.is_interface_pointer( held ) )
        {
            continue;
        }

        const std::optional<Origin> origin = origin_of( ast, types, func, assigned, Local_id { p } );
        if( !origin )
        {
            continue;
        }

        const Type_id        interface = table.get( held ).element;
        std::vector<Node_id> methods;
        for( const Statement* call : calls[p] )
        {
            methods.push_back( types.conformances().method_for( table, origin->type, interface, func.rvalue( *call ).callee ) );
        }

        if( std::ranges::any_of( methods, []( Node_id method ) { return !method.is_valid(); } ) )
        {
            continue; // the compiler supplies the conformance
        }

        // A copied pointer keeps its own type, so the copy forwards; C takes it where `const` is asked.
        const Type_id              pointer   = origin->made.kind == Rvalue_kind::Use
                                                   ? origin->made.type
                                                   : table.pointer_to( origin->type, table.get( held ).const_element );
        const std::vector<Type_id> arguments = [&]
        {
            const auto& written = table.get( origin->type ).arguments;
            return std::vector<Type_id>( written.begin(), written.end() );
        }();

        Rvalue& made                     = func.rvalue( *only[p] );
        made                             = origin->made;
        made.type                        = pointer;
        func.locals[Local_id { p }].type = pointer;

        for( u32 i = 0; i < calls[p].size(); ++i )
        {
            Rvalue& value           = func.rvalue( *calls[p][i] );
            value.callee            = methods[i];
            value.type_arguments    = arguments;
            value.arguments[0].type = pointer;
        }

        changed = true;
    }

    return changed;
}

std::vector<std::vector<u32>> dispatch_edges( const std::vector<Function>& functions, const Ast& ast, Types& types )
{
    std::vector<std::vector<u32>> edges( functions.size() );

    for( u32 f = 0; f < functions.size(); ++f )
    {
        for( const Statement& statement : functions[f].statements )
        {
            if( statement.kind != Statement_kind::Assign )
            {
                continue;
            }

            const auto [implementer, interface] =
                interface_conversion( ast, types, functions[f], functions[f].rvalue( statement ) );
            const Conformance* conformance =
                implementer.is_valid() ? types.conformances().of( types.table(), implementer, interface ) : nullptr;

            if( conformance == nullptr )
            {
                continue;
            }

            const auto& arguments = types.table().get( implementer ).arguments;
            for( u32 c = 0; c < functions.size(); ++c )
            {
                const bool answers =
                    std::ranges::find( conformance->methods, functions[c].declaration ) != conformance->methods.end();
                if( answers && std::ranges::equal( functions[c].type_arguments, arguments ) )
                {
                    edges[f].push_back( c );
                }
            }
        }
    }

    return edges;
}

} // namespace keel

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include "check/pipeline_test_support.h"
#include "ir/verify.h"

namespace keel
{
namespace
{

constexpr std::string_view k_types =
    "interface shape { i32 area() const; };\n"
    "class Box implements shape { public i32 v; public Box( i32 n ) { v = n; } public i32 area() const { return v; } };\n"
    "i32 dyn( const ref shape s ) { return s.area(); }\n"
    "i32 main() { return 0; }\n";

// Optimised, as a release build lowers it. The function under test is always the first.
struct Devirtualised : Compiled
{
    explicit Devirtualised( std::string_view source )
        : Compiled(
              std::string( source ) + std::string( k_types ),
              Stage::Elaborated,
              Build { .optimise = true, .inlining = Inline_weights {} }
          )
    {
    }

    std::size_t calls() const
    {
        const Function& first = functions.front();
        return static_cast<std::size_t>( std::ranges::count_if(
            first.statements,
            [&]( const Statement& s )
            { return s.kind == Statement_kind::Assign && first.rvalue( s ).kind == Rvalue_kind::Call; }
        ) );
    }
};

} // namespace

// Through a `ref`, a call taking one, and a pointer: each becomes `Box::area` and inlines.
TEST_CASE( "devirtualise_calls_calls_the_known_implementer", "[opt][devirtualise]" )
{
    for( const char* source : {
             "i32 f( i32 a ) { Box b = Box( a ); const ref shape s = b; return s.area(); }\n",
             "i32 f( i32 a ) { Box b = Box( a ); return dyn( b ); }\n",
             "i32 f( i32 a ) { Box b = Box( a ); shape* p = &b; return ( *p ).area(); }\n",
         } )
    {
        const Devirtualised p( source );
        INFO( source );
        REQUIRE( p.clean() );
        REQUIRE( verify( p.functions.front() ).empty() );
        REQUIRE( p.calls() == 0 );
    }
}

// A pointer at one of two keeps its vtable call.
TEST_CASE( "devirtualise_calls_keeps_a_pointer_assigned_twice", "[opt][devirtualise]" )
{
    const Devirtualised p( "i32 f( i32 a ) { Box b = Box( a ); Box c = Box( 2 ); shape* p = &b; if( a > 0 ) { p = &c; } "
                           "return ( *p ).area(); }\n" );
    REQUIRE( p.clean() );
    REQUIRE( p.calls() == 1 );
}

} // namespace keel
#endif
