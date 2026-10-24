// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <string>
#include "ast/ast.h"
#include "common/interner.h"
#include "sema/aggregates.h"
#include "sema/reporter.h"
#include "sema/types_builder.h"

namespace keel::sema
{

// D70: what an extern's signature may hand C, by value or behind a pointer.
class Extern_types
{
public:
    Extern_types(
        const Ast& ast, const Interner& interner, const Types_builder& types, const Aggregates& aggregates, Reporter& reporter
    )
        : ast_( ast ),
          interner_( interner ),
          types_( types ),
          table_( types.table() ),
          aggregates_( aggregates ),
          reporter_( reporter )
    {
    }

    // One error per parameter or return C cannot read.
    void check( Node_id extern_decl );

private:
    // What C cannot read, and the innermost struct field holding it.
    struct Foreign
    {
        Type_id part;
        Type_id holder;
        Node_id field;
    };

    Foreign foreign_part( Type_id type ) const; // an invalid `part` when C can read all of it
    Foreign foreign_signature( Type_id function ) const;
    Foreign foreign_parameter( Type_id type, Param_mode mode ) const;
    bool    crosses_by_pointer( Type_id type ) const;

    std::string help( const Foreign& found ) const;

    const Ast&           ast_;
    const Interner&      interner_;
    const Types_builder& types_;
    const Type_table&    table_;
    const Aggregates&    aggregates_;
    Reporter&            reporter_;
};

} // namespace keel::sema
