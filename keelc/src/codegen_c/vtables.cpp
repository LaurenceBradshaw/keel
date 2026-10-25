// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_c/vtables.h"
#include <algorithm>
#include <utility>
#include "ir/place_type.h"

namespace keel
{

namespace
{

// The implementer and interface of a conversion to an interface pointer or `box<Shape>`, or neither
// when the value is one already: `&(*s)` with `s` a `Shape*` is `s` again, and needs no table.
std::pair<Type_id, Type_id> converted_from( const Ast& ast, Types& types, const Function& function, const Rvalue& value )
{
    const Type_table& table = types.table();

    Type_id from {};
    Type_id interface {};

    if( table.is_interface_pointer( value.type ) )
    {
        interface = table.get( value.type ).element;

        if( value.kind == Rvalue_kind::Address_of )
        {
            from = place_type( ast, types, function, value.a.place );
        }
        else if( value.kind == Rvalue_kind::Cast && table.is_pointer( value.a.type ) )
        {
            from = table.get( value.a.type ).element;
        }
    }
    else if( value.kind == Rvalue_kind::Cast && table.is_box( value.type ) && table.is_box( value.a.type ) )
    {
        interface = table.get( value.type ).arguments[0];
        from      = table.get( value.a.type ).arguments[0];
    }

    if( !from.is_valid() || !table.is_interface( interface ) || table.is_interface( from ) )
    {
        return {};
    }

    return { from, interface };
}

} // namespace

std::vector<Vtable> vtables_needed( const Ast& ast, Types& types, std::span<const Function> functions )
{
    std::vector<Vtable> needed;

    for( const Function& function : functions )
    {
        for( const Statement& statement : function.statements )
        {
            if( statement.kind != Statement_kind::Assign )
            {
                continue;
            }

            const auto [implementer, interface] = converted_from( ast, types, function, statement.value );

            if( !implementer.is_valid() )
            {
                continue;
            }

            const bool seen = std::any_of(
                needed.begin(),
                needed.end(),
                [&]( const Vtable& vtable ) { return vtable.implementer == implementer && vtable.interface == interface; }
            );

            if( seen )
            {
                continue;
            }

            const Conformance* conformance = types.conformances().of( types.table(), implementer, interface );

            if( conformance == nullptr )
            {
                continue; // the checker allows no conversion without one
            }

            needed.push_back( {
                .implementer  = implementer,
                .interface    = interface,
                .requirements = conformance->requirements,
                .methods      = conformance->methods,
            } );
        }
    }

    return needed;
}

} // namespace keel
