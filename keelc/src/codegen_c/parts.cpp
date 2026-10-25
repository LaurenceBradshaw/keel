// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_c/parts.h"
#include <algorithm>
#include "codegen_c/vtables.h"
#include "ir/place_type.h"

namespace keel
{

Parts::Parts( const Ast& ast, Types& types, const Module_graph& modules, File_id input, const std::vector<Function>& functions )
    : ast_( ast ),
      functions_( functions ),
      order_( modules.dependencies_first( input ) ),
      reaches_( functions.size() ),
      owners_( functions.size() )
{
    for( u32 f = 0; f < functions.size(); ++f )
    {
        by_declaration_.emplace( functions[f].declaration.v, f );
    }

    const Type_table& table = types.table();

    for( u32 f = 0; f < functions.size(); ++f )
    {
        const auto reach = [&]( u32 callee )
        {
            if( callee != k_invalid_kir_id && std::ranges::find( reaches_[f], callee ) == reaches_[f].end() )
            {
                reaches_[f].push_back( callee );
            }
        };

        for( const Statement& statement : functions[f].statements )
        {
            const Rvalue& value = statement.value;

            if( statement.kind == Statement_kind::Assign &&
                ( value.kind == Rvalue_kind::Call || value.kind == Rvalue_kind::Function_address ) )
            {
                reach( find( value.callee, value.type_arguments ) );
            }
            else if( statement.kind == Statement_kind::Drop && value.callee.is_valid() )
            {
                reach( find( value.callee, value.type_arguments ) );
            }
            else if( statement.kind == Statement_kind::Drop )
            {
                // An enum's, a union's or an array's synthesised destructor; an interface's is its vtable's.
                const Type_id dropped = place_type( ast, types, functions[f], statement.place );

                if( table.is_enum( dropped ) )
                {
                    reach( find( table.get( dropped ).declaration, table.get( dropped ).arguments ) );
                }
                else if( table.is_union( dropped ) || table.is_array( dropped ) )
                {
                    reach( find( {}, std::span( &dropped, 1 ) ) );
                }
            }
        }

        for( const Vtable& vtable : vtables_needed( ast, types, std::span( &functions[f], 1 ) ) )
        {
            for( const Node_id method : vtable.methods )
            {
                reach( find( method, table.get( vtable.implementer ).arguments ) );
            }

            reach( find( {}, std::span( &vtable.implementer, 1 ) ) );
        }
    }

    const auto declared = [&]( u32 f ) { return functions[f].declaration.is_valid() && functions[f].type_arguments.empty(); };

    for( u32 f = 0; f < functions.size(); ++f )
    {
        if( declared( f ) )
        {
            owners_[f] = of( functions[f].declaration );
        }
    }

    // Each module in turn claims what its own functions reach through instances not yet claimed.
    for( const File_id part : order_ )
    {
        std::vector<bool> seen( functions.size(), false );
        std::vector<u32>  stack;

        for( u32 f = 0; f < functions.size(); ++f )
        {
            if( declared( f ) && owners_[f] == part )
            {
                stack.push_back( f );
            }
        }

        while( !stack.empty() )
        {
            const u32 f = stack.back();
            stack.pop_back();

            for( const u32 callee : reaches_[f] )
            {
                if( seen[callee] || declared( callee ) )
                {
                    continue;
                }

                seen[callee] = true;

                if( !owners_[callee].is_valid() )
                {
                    owners_[callee] = part;
                }

                stack.push_back( callee );
            }
        }
    }

    // A destructor queued for a composite nothing drops.
    for( File_id& owner : owners_ )
    {
        if( !owner.is_valid() )
        {
            owner = input;
        }
    }
}

File_id Parts::of( const Function& function ) const
{
    if( function.declaration.is_valid() && function.type_arguments.empty() )
    {
        return of( function.declaration );
    }

    return owners_[find( function.declaration, function.type_arguments )];
}

File_id Parts::of( Node_id declaration ) const
{
    return ast_.span( declaration ).file;
}

std::span<const u32> Parts::reaches( u32 function ) const
{
    return reaches_[function];
}

std::span<const File_id> Parts::order() const
{
    return order_;
}

File_id Parts::input() const
{
    return order_.back();
}

u32 Parts::find( Node_id declaration, std::span<const Type_id> type_arguments ) const
{
    const auto [first, last] = by_declaration_.equal_range( declaration.v );

    for( auto it = first; it != last; ++it )
    {
        if( std::ranges::equal( functions_[it->second].type_arguments, type_arguments ) )
        {
            return it->second;
        }
    }

    return k_invalid_kir_id;
}

} // namespace keel
