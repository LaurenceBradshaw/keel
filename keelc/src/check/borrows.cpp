// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/borrows.h"

namespace keel
{

std::vector<Borrow> borrows_of( const Function& func, const std::vector<bool>& addressing, const Literal_pool& literals )
{
    std::vector<Borrow> borrows( func.locals.size() );
    std::vector<bool>   chosen( func.locals.size(), false );

    // First, so `&(*x)` through a chosen `ref` binding finds its arms.
    for( const Local_id local : func.chosen )
    {
        chosen[local.v] = true;
    }

    for( const Statement& statement : func.statements )
    {
        if( statement.kind == Statement_kind::Assign && statement.place.is_whole_local() && chosen[statement.place.local.v] &&
            statement.value.kind == Rvalue_kind::Use && statement.value.a.kind == Operand_kind::Copy &&
            statement.value.a.place.is_whole_local() )
        {
            borrows[statement.place.local.v].choices.push_back( statement.value.a.place.local );
        }
    }

    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign || statement.place.is_global() || statement.place.num_projections != 0 )
        {
            continue;
        }

        Borrow&       borrow = borrows[statement.place.local.v];
        const Rvalue& value  = statement.value;
        const Place&  source = value.a.place;

        borrow.assignments++;
        borrow.of       = Local_id {};
        borrow.bound    = false;
        borrow.through  = nullptr;
        borrow.via      = Local_id {};
        borrow.constant = std::nullopt;
        borrow.copy_of  = Local_id {};

        if( !chosen[statement.place.local.v] )
        {
            borrow.choices.clear();
        }

        if( borrow.assignments != 1 || source.is_global() || chosen[statement.place.local.v] )
        {
            continue;
        }

        if( value.kind == Rvalue_kind::Use && value.a.kind == Operand_kind::Constant && value.a.constant.is_valid() &&
            literals.kind( value.a.constant ) == Literal_kind::Integer )
        {
            borrow.constant = literals.integer( value.a.constant );
        }

        const bool derefs =
            source.num_projections != 0 && func.projections[source.first_projection].kind == Projection_kind::Deref;
        // A call's or a constant's `a` names no local.
        const Borrow inner = source.local.is_valid() ? borrows[source.local.v] : Borrow {};

        if( value.kind == Rvalue_kind::Address_of )
        {
            borrow.at      = statement.span;
            borrow.bound   = true;
            borrow.purpose = value.address_purpose;

            // `&(*_t)` after `_t = call operator[](...)`: an element, reached through the call.
            if( derefs && inner.through != nullptr )
            {
                borrow.through = inner.through;
            }
            // Through a chosen `ref` binding, each of its arms.
            else if( derefs && !inner.choices.empty() )
            {
                borrow.choices = inner.choices;
                borrow.via     = source.local;
            }
            // Through a `ref` binding, `&(*r)`, it is the whole of what `r` borrows.
            else if( derefs && inner.of.is_valid() )
            {
                borrow.of    = inner.of;
                borrow.place = Place { .local = inner.of };
                borrow.via   = inner.via.is_valid() ? inner.via : source.local;
            }
            else
            {
                borrow.of    = source.local;
                borrow.place = source;
            }
        }
        // An address computed from an address (a cast, `p + i`, a run's data, a sub-run) borrows what it does.
        // An integer's `+` copies an empty borrow.
        else if( ( value.kind == Rvalue_kind::Use || value.kind == Rvalue_kind::Cast || value.kind == Rvalue_kind::Run_of ||
                   value.kind == Rvalue_kind::Run_data ||
                   ( value.kind == Rvalue_kind::Binary && value.op == Token_kind::Plus ) ) &&
                 value.a.kind == Operand_kind::Copy && source.num_projections == 0 )
        {
            borrow.of      = inner.of;
            borrow.place   = inner.place;
            borrow.at      = inner.at;
            borrow.purpose = inner.purpose;
            borrow.through = inner.through;
            borrow.via     = inner.via;
            borrow.choices = inner.choices;
        }
        else if( value.kind == Rvalue_kind::Call || value.kind == Rvalue_kind::Indirect_call )
        {
            borrow.at = statement.span;

            // A value that can hold no address cannot reach into the receiver.
            if( addressing[statement.place.local.v] )
            {
                borrow.through = &value;
            }
        }
    }

    // Bound like a `ref` binding, at and for what its first arm is.
    for( const Local_id local : func.chosen )
    {
        Borrow& borrow = borrows[local.v];

        if( !borrow.choices.empty() )
        {
            borrow.bound   = true;
            borrow.at      = borrows[borrow.choices.front().v].at;
            borrow.purpose = borrows[borrow.choices.front().v].purpose;
        }
    }

    // An address may be taken before the one assignment, and `&init` counts.
    std::vector<bool> addressed( func.locals.size(), false );
    for( const Statement& statement : func.statements )
    {
        if( statement.kind != Statement_kind::Assign || statement.value.kind != Rvalue_kind::Address_of ||
            statement.value.a.place.is_global() || statement.value.a.place.num_projections != 0 )
        {
            continue;
        }

        addressed[statement.value.a.place.local.v] = true;
    }

    for( std::size_t l = 0; l < borrows.size(); ++l )
    {
        if( addressed[l] )
        {
            borrows[l].constant = std::nullopt;
        }
    }

    for( const Statement& statement : func.statements )
    {
        const Place& place  = statement.place;
        const Place& source = statement.value.a.place;

        if( statement.kind != Statement_kind::Assign || place.is_global() || place.num_projections != 0 ||
            borrows[place.local.v].assignments != 1 || statement.value.kind != Rvalue_kind::Use ||
            statement.value.a.kind != Operand_kind::Copy || source.is_global() || source.num_projections != 0 ||
            addressed[source.local.v] )
        {
            continue;
        }

        borrows[place.local.v].copy_of = source.local;
    }

    return borrows;
}

bool overlaps( const Function& func, const std::vector<Borrow>& borrows, const Place& a, const Place& b )
{
    if( a.local != b.local )
    {
        return false;
    }

    u32 min_projections = std::min( a.num_projections, b.num_projections );
    for( u32 k = 0; k < min_projections; ++k )
    {
        const Projection& pa = func.projections[a.first_projection + k];
        const Projection& pb = func.projections[b.first_projection + k];

        if( pa.kind != pb.kind )
        {
            return false;
        }

        if( pa.kind == Projection_kind::Field && pa.field != pb.field )
        {
            return false;
        }

        if( pa.kind == Projection_kind::Member && pa.member != pb.member )
        {
            return false;
        }

        if( pa.kind == Projection_kind::Index && borrows[pa.index.v].constant && borrows[pb.index.v].constant &&
            borrows[pa.index.v].constant != borrows[pb.index.v].constant )
        {
            return false;
        }
    }

    return true;
}

bool may_differ( const Function& func, const std::vector<Borrow>& borrows, const Place& a, const Place& b )
{
    const u32 min_projections = std::min( a.num_projections, b.num_projections );
    for( u32 k = 0; k < min_projections; ++k )
    {
        const Projection& pa = func.projections[a.first_projection + k];
        const Projection& pb = func.projections[b.first_projection + k];

        if( pa.kind != Projection_kind::Index || pb.kind != Projection_kind::Index )
        {
            continue;
        }

        const Borrow& ia = borrows[pa.index.v];
        const Borrow& ib = borrows[pb.index.v];

        if( pa.index != pb.index && !( ia.copy_of.is_valid() && ia.copy_of == ib.copy_of ) && ( !ia.constant || !ib.constant ) )
        {
            return true;
        }
    }
    return false;
}

u32 reaches_depth( const Function& func, const std::vector<Borrow>& borrows, const Borrow& element, const Place& whole )
{
    const std::span<const Operand> arguments(
        func.operands.data() + element.through->first_argument, element.through->argument_count
    );

    u32 depth = 0;

    for( const Operand& operand : arguments )
    {
        if( operand.kind != Operand_kind::Copy || operand.place.is_global() || operand.place.num_projections != 0 )
        {
            continue;
        }

        for_each_choice(
            borrows,
            borrows[operand.place.local.v],
            [&]( const Borrow& inner )
            {
                if( inner.through != nullptr )
                {
                    const u32 inner_depth = reaches_depth( func, borrows, inner, whole );

                    if( inner_depth != 0 )
                    {
                        depth = std::max( depth, inner_depth + 1 );
                    }
                }
                else if( inner.of.is_valid() && overlaps( func, borrows, inner.place, whole ) )
                {
                    depth = std::max( depth, 1u );
                }
            }
        );
    }

    return depth;
}

bool reaches( const Function& func, const std::vector<Borrow>& borrows, const Borrow& element, const Place& whole )
{
    return reaches_depth( func, borrows, element, whole ) != 0;
}

bool borrows_from( const Function& func, const std::vector<Borrow>& borrows, const Borrow& loan, const Place& place )
{
    return any_choice(
        borrows,
        loan,
        [&]( const Borrow& choice )
        {
            if( choice.of.is_valid() )
            {
                return overlaps( func, borrows, choice.place, place );
            }

            return choice.through != nullptr && reaches( func, borrows, choice, place );
        }
    );
}

} // namespace keel
