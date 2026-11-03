// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/place_type.h"

namespace keel
{

Type_id place_type( const Ast& ast, Types& types, const Function& function, const Place& place )
{
    Type_id type = place.is_global() ? types.type_of( place.global ) : function.locals[place.local].type;

    for( const Projection& projection : function.projections_of( place ) )
    {
        type = projection.kind == Projection_kind::Field
                   ? field_type( ast, types.table(), type, projection.field, types.recorded() )
               : projection.kind == Projection_kind::Member ? projection.member
                                                            : types.table().get( type ).element;
    }

    return type;
}

std::pair<Type_id, Type_id> interface_conversion( const Ast& ast, Types& types, const Function& function, const Rvalue& value )
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

} // namespace keel
