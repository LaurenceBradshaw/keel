// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <string>
#include <string_view>
#include "ast/ast.h"
#include "codegen_c/mangle.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "common/types.h"
#include "parse/module_graph.h"
#include "sema/type_checker.h"

namespace keel
{
struct Spelling
{
    const Ast&            ast;
    Types&                types; // mutable only for its table: an instance's symbol interns its types
    const Interner&       interner;
    const Module_graph&   modules;
    const Source_manager& sm;

    std::string type( Type_id type ) const; // "int32_t", "struct kl__Point", "int32_t*"
    std::string structure( Type_id type ) const;

    // §6.7's interfaces: a `Shape*` is a struct of the object's address and its vtable's, and each
    // implementer seen as one gets a vtable of thunks, one per slot.
    std::string interface_reference( Type_id interface ) const; // "struct kl__Shape__ref"
    std::string run_reference( Type_id run ) const;
    std::string parameter( Type_id type, std::string_view name ) const;
    std::string argument( Type_id type, std::string_view name ) const;
    std::string vtable_type( Type_id interface ) const; // "struct kl__Shape__vtable"
    std::string vtable( Type_id implementer, Type_id interface ) const;
    std::string thunk( Type_id implementer, Type_id interface, Node_id requirement ) const;
    // Not `destructor_of`: an implementer's `~` runs its body only, and this drops its fields too.
    std::string drop_function( Type_id implementer ) const; // "kl__Circle__drop"
    std::string field( Node_id declaration ) const;
    // The declaration alone no longer names a function: one generic is emitted once per set of
    // type arguments, so they are part of the symbol.
    std::string function( Node_id declaration, std::span<const Type_id> type_arguments = {} ) const;
    // A vtable slot: the requirement's name and parameters, so an overloaded requirement has two.
    std::string slot( Node_id requirement ) const;
    // A global or static field. `owner` is the instance whose static this is, invalid otherwise.
    std::string global( Node_id declaration, Type_id owner = {} ) const;

    // The symbol a Drop calls. A Drop always names a type with a destructor of its own, or an enum
    // with its synthesised one - the lowerer expands any other compound into per-field drops.
    std::string destructor_of( Type_id type ) const;
    // The types alone: `int32_t, uint8_t`. That is all a prototype needs, and all a definition can
    // use - the body names its parameters by Local_id, so nothing here may name them at all.
    std::string parameter_types( Node_id declaration ) const;

    // What C sees a function hand back. Not the declared type when the function returns a binding:
    // that travels as an address, so the definition, the prototype and the KIR return slot must all
    // say the pointer or C is told one thing and given another.
    std::string return_type( Node_id declaration ) const;

    // A file-scope variable's whole definition. Its initialiser stays an expression rather than a
    // value: a C file-scope initialiser must be one constant expression, and there is nowhere at
    // file scope to put the temporaries three-address form would need.
    std::string global_definition( Node_id declaration, Type_id owner = {} ) const;

    Mangled_owner owner( Node_id decl ) const;
};

// How C is told what kind of number it is looking at. Used by the KIR emitter for a constant
// operand, and by global_definition for a folded initialiser.

// fmt's default is the shortest form that round-trips, and a value like 1.0 prints as "1", which C
// would read as an int.
std::string c_float( f64 value );

// The suffix stops C choosing a signed type too narrow to hold the value. Deliberately *not* a
// cast to the literal's own type: `-2147483648` is a negation of 2147483648, and casting that
// first would overflow before the minus ran. The temporary each operation writes into carries the
// explicit C type instead.
std::string c_integer( u64 value );

std::string c_string( std::string_view str );
} // namespace keel
