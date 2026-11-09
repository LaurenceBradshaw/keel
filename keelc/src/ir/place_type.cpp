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

std::pair<Type_id, Type_id> interface_conversion( const Type_table& table, const Rvalue& value )
{
    if( value.kind != Rvalue_kind::Make_interface )
    {
        return {};
    }

    return { table.get( value.a.type ).element, table.get( value.type ).element };
}

} // namespace keel
