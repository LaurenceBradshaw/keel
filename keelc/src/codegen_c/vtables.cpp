// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_c/vtables.h"
#include <algorithm>

namespace keel
{

Type_id place_type( const Ast& ast, Types& types, const Function& function, const Place& place )
{
    Type_id type = place.is_global() ? types.type_of( place.global ) : function.locals[place.local.v].type;

    for( u32 i = 0; i < place.num_projections; ++i )
    {
        const Projection& projection = function.projections[place.first_projection + i];

        type = projection.kind == Projection_kind::Field
                   ? field_type( ast, types.table(), type, projection.field, types.recorded() )
               : projection.kind == Projection_kind::Member ? projection.member
                                                            : types.table().get( type ).element;
    }

    return type;
}

namespace
{

// The concrete type a conversion to an interface pointer starts from, or invalid when the value is
// one already: `&(*s)` with `s` a `Shape*` is `s` again, and needs no table.
Type_id converted_from( const Ast& ast, Types& types, const Function& function, const Rvalue& value )
{
    const Type_table& table = types.table();

    if( !table.is_interface_pointer( value.type ) )
    {
        return Type_id {};
    }

    Type_id from {};

    if( value.kind == Rvalue_kind::Address_of )
    {
        from = place_type( ast, types, function, value.a.place );
    }
    else if( value.kind == Rvalue_kind::Cast && table.is_pointer( value.a.type ) )
    {
        from = table.get( value.a.type ).element;
    }

    return from.is_valid() && !table.is_interface( from ) ? from : Type_id {};
}

} // namespace

std::vector<Vtable> vtables_needed( const Ast& ast, Types& types, const std::vector<Function>& functions )
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

            const Type_id implementer = converted_from( ast, types, function, statement.value );

            if( !implementer.is_valid() )
            {
                continue;
            }

            const Type_id interface = types.table().get( statement.value.type ).element;

            const bool seen = std::any_of(
                needed.begin(),
                needed.end(),
                [&]( const Vtable& vtable ) { return vtable.implementer == implementer && vtable.interface == interface; }
            );

            if( seen )
            {
                continue;
            }

            const Conformance* conformance =
                types.conformances().find( types.table().get( implementer ).declaration, interface );

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
