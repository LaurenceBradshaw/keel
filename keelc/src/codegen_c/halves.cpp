// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_c/halves.h"
#include <algorithm>

namespace keel
{

Halves::Halves( const Ast& ast, const Type_table& table, const Imports& imports )
    : ast_( ast ),
      table_( table ),
      imports_( imports )
{
}

C_part Halves::of( Node_id declaration ) const
{
    return ast_.span( declaration ).file == imports_.prelude_file() ? C_part::Prelude : C_part::Program;
}

C_part Halves::of( Type_id type ) const
{
    if( !type.is_valid() )
    {
        return C_part::Prelude;
    }

    const Type& described = table_.get( type );
    const auto  program   = [&]( Type_id part ) { return of( part ) == C_part::Program; };

    if( table_.is_union( type ) || ( described.declaration.is_valid() && of( described.declaration ) == C_part::Program ) ||
        program( described.element ) || std::ranges::any_of( described.arguments, program ) )
    {
        return C_part::Program;
    }

    return C_part::Prelude;
}

C_part Halves::of( const Function& function ) const
{
    if( !function.declaration.is_valid() || of( function.declaration ) == C_part::Program ||
        std::ranges::any_of( function.type_arguments, [&]( Type_id argument ) { return of( argument ) == C_part::Program; } ) )
    {
        return C_part::Program;
    }

    return C_part::Prelude;
}

C_part Halves::of( const Vtable& vtable ) const
{
    const bool declared = table_.get( vtable.implementer ).declaration.is_valid();

    return declared && of( vtable.implementer ) == C_part::Prelude && of( vtable.interface ) == C_part::Prelude
               ? C_part::Prelude
               : C_part::Program;
}

bool Halves::writes_in( Type_id signature, C_part part ) const
{
    return part == C_part::Program || of( signature ) == C_part::Prelude;
}

} // namespace keel
