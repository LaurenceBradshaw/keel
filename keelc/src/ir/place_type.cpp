// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/place_type.h"

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

} // namespace keel
