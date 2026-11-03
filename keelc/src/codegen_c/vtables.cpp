// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_c/vtables.h"
#include <algorithm>
#include <utility>
#include "ir/place_type.h"

namespace keel
{

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

            const auto [implementer, interface] = interface_conversion( ast, types, function, function.rvalue( statement ) );

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
