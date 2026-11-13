// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/purity.h"
#include "ir/loops.h"

namespace keel
{
Purity::Purity( const std::vector<Function>& functions, const Ast& ast )
    : functions_( functions ),
      ast_( ast ),
      pure_( functions.size(), false ),
      removable_( functions.size(), false )
{
    for( u32 f = 0; f < functions.size(); ++f )
    {
        const Function& func = functions[f];
        by_declaration_[func.declaration.v].push_back( f );

        for( const Statement& statement : func.statements )
        {
            if( statement.kind != Statement_kind::Storage_live && statement.kind != Statement_kind::Storage_dead &&
                statement.place.is_global() )
            {
                written_.insert( statement.place.global.v );
            }
            if( !statement.value.is_valid() )
            {
                continue;
            }

            const Rvalue& value = func.rvalue( statement );
            for_each_operand(
                value,
                [&]( const Operand& operand )
                {
                    const bool lent = value.kind == Rvalue_kind::Address_of && value.address_purpose != Address_purpose::Read;
                    if( operand.has_place() && operand.place.is_global() && ( lent || operand.kind == Operand_kind::Move ) )
                    {
                        written_.insert( operand.place.global.v );
                    }
                }
            );
        }
    }
}

std::optional<u32> Purity::callee( const Rvalue& call ) const
{
    if( call.kind != Rvalue_kind::Call || call.globals != Globals_call::None )
    {
        return std::nullopt;
    }

    const auto instances = by_declaration_.find( call.callee.v );
    if( instances == by_declaration_.end() )
    {
        return std::nullopt;
    }

    for( const u32 c : instances->second )
    {
        if( names( call, functions_[c] ) )
        {
            return c;
        }
    }

    return std::nullopt;
}

bool Purity::pure( const Rvalue& call ) const
{
    const std::optional<u32> c = callee( call );
    return c && pure_[*c];
}

bool Purity::removable( const Rvalue& call ) const
{
    const std::optional<u32> c = callee( call );
    return c && removable_[*c];
}

bool Purity::pure_body( const Function& func ) const
{
    if( func.blocks.empty() || func.diverges )
    {
        return false;
    }

    const auto readable = [&]( const Operand& operand )
    {
        if( !operand.has_place() )
        {
            return true;
        }
        if( operand.place.is_global() )
        {
            return ast_.is_const_binding( operand.place.global ) && !written_.contains( operand.place.global.v );
        }
        return !has_deref_projection( func, operand.place );
    };

    for( const Statement& statement : func.statements )
    {
        if( statement.kind == Statement_kind::Storage_live || statement.kind == Statement_kind::Storage_dead )
        {
            continue;
        }
        if( statement.kind == Statement_kind::Drop || statement.place.is_global() ||
            has_deref_projection( func, statement.place ) )
        {
            return false;
        }

        const Rvalue& value = func.rvalue( statement );
        switch( value.kind )
        {
        case Rvalue_kind::Call:
            if( !pure( value ) )
            {
                return false;
            }
            break;
        case Rvalue_kind::Indirect_call:
        case Rvalue_kind::Allocate:
        case Rvalue_kind::Release:
        case Rvalue_kind::Field_read:
            return false;
        default:
            break;
        }

        if( any_operand( value, [&]( const Operand& operand ) { return !readable( operand ); } ) )
        {
            return false;
        }
    }

    for( const Block& block : func.blocks )
    {
        const Terminator& terminator = block.terminator;
        if( !readable( terminator.condition ) || !readable( terminator.message ) || !readable( terminator.at ) )
        {
            return false;
        }
    }

    return true;
}

bool Purity::returns( const Function& func ) const
{
    // An edge back to a block reverse postorder places no later is a cycle.
    const Dominators      dominators = find_dominators( func );
    std::vector<Block_id> targets;
    for( const Block_id b : func.blocks.ids() )
    {
        const Block& block = func.blocks[b];
        if( !dominators.idom[b.v].is_valid() )
        {
            continue;
        }
        if( block.terminator.kind == Terminator_kind::Panic )
        {
            return false;
        }

        targets.clear();
        successors( block.terminator, targets );
        for( const Block_id target : targets )
        {
            if( dominators.order[target.v] <= dominators.order[b.v] )
            {
                return false;
            }
        }
    }

    for( const Statement& statement : func.statements )
    {
        if( statement.kind == Statement_kind::Assign && func.rvalue( statement ).kind == Rvalue_kind::Call &&
            !removable( func.rvalue( statement ) ) )
        {
            return false;
        }
    }

    return true;
}

void Purity::decide( const std::vector<u32>& group )
{
    // Assumed of every member, then refuted until none changes: two that call only each other stay pure.
    for( const u32 f : group )
    {
        pure_[f] = true;
    }
    for( bool changed = true; changed; )
    {
        changed = false;
        for( const u32 f : group )
        {
            if( pure_[f] && !pure_body( functions_[f] ) )
            {
                pure_[f] = false;
                changed  = true;
            }
        }
    }

    // A group of several recurses; one calling itself fails on its own call.
    if( group.size() == 1 )
    {
        removable_[group[0]] = pure_[group[0]] && returns( functions_[group[0]] );
    }
}
} // namespace keel
