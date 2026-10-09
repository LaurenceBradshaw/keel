// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check/borrows.h"

namespace keel
{

std::vector<Borrow> borrows_of( const Function& func )
{
    std::vector<Borrow> borrows( func.locals.size() );

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
        borrow.of      = Local_id {};
        borrow.bound   = false;
        borrow.through = nullptr;

        if( borrow.assignments != 1 || source.is_global() )
        {
            continue;
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
            // Through a `ref` binding, `&(*r)`, it is the whole of what `r` borrows.
            else if( derefs && inner.of.is_valid() )
            {
                borrow.of    = inner.of;
                borrow.place = Place { .local = inner.of };
            }
            else
            {
                borrow.of    = source.local;
                borrow.place = source;
            }
        }
        // A conversion of the address, such as to a pointer to `const`, still points at the same local.
        else if( ( value.kind == Rvalue_kind::Use || value.kind == Rvalue_kind::Cast ) && value.a.kind == Operand_kind::Copy &&
                 source.num_projections == 0 )
        {
            borrow.of      = inner.of;
            borrow.place   = inner.place;
            borrow.at      = inner.at;
            borrow.purpose = inner.purpose;
            borrow.through = inner.through;
        }
        else if( value.kind == Rvalue_kind::Call || value.kind == Rvalue_kind::Indirect_call )
        {
            borrow.through = &value;
        }
    }

    return borrows;
}

bool overlaps( const Function& func, const Place& a, const Place& b )
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
    }

    return true;
}

bool reaches( const Function& func, const std::vector<Borrow>& borrows, const Borrow& element, const Place& whole )
{
    const std::span<const Operand> arguments(
        func.operands.data() + element.through->first_argument, element.through->argument_count
    );

    for( const Operand& operand : arguments )
    {
        if( operand.kind != Operand_kind::Copy || operand.place.is_global() || operand.place.num_projections != 0 )
        {
            continue;
        }

        const Borrow& inner = borrows[operand.place.local.v];

        if( inner.through != nullptr )
        {
            if( reaches( func, borrows, inner, whole ) )
            {
                return true;
            }
        }
        else if( inner.of.is_valid() && overlaps( func, inner.place, whole ) )
        {
            return true;
        }
    }

    return false;
}

} // namespace keel
