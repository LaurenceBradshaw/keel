// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include "ast/ast.h"
#include "codegen_c/vtables.h"
#include "ir/kir.h"
#include "parse/module_graph.h"
#include "sema/type.h"

namespace keel
{

// The prelude's C is a file of its own, which the program's includes: the prelude sees nothing of
// the program, so its half compiles alone.
enum class C_part : u8
{
    Program,
    Prelude,
};

// Which C file each thing goes in (D46). The program's includes the prelude's, never the reverse:
//
//   declaration           the file it is written in
//   aggregate, interface  the prelude's if declared there and every type argument is
//   union                 the program's: nothing in the prelude names one
//   array, run            its element's
//   function type         its parameters' and result's, and the program writes them all again
//   function              the prelude's if declared there and every type argument is; a
//                         synthesised one is the program's
//   vtable                the prelude's if its implementer and interface are both declared there
//
// A built-in type is the prelude's. The emitter, --dump-kir and the test fixture all ask here.
class Halves
{
public:
    Halves( const Ast& ast, const Type_table& table, const Module_graph& modules );

    C_part of( Node_id declaration ) const;
    C_part of( Type_id type ) const;
    C_part of( const Function& function ) const;
    C_part of( const Vtable& vtable ) const;

    // The one thing both halves write: a function type, typedef'd twice, which C11 allows.
    bool writes_in( Type_id signature, C_part part ) const;

private:
    const Ast&          ast_;
    const Type_table&   table_;
    const Module_graph& modules_;
};

} // namespace keel
