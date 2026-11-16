// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <utility>
#include <vector>
#include "ast/ast.h"
#include "ast/node.h"
#include "common/diagnostics.h"
#include "common/interner.h"
#include "common/source_manager.h"
#include "parse/module_graph.h"
#include "sema/prelude_items.h"

namespace keel
{

struct Loaded_state;

// A use in `from` may name `decl` when it imports the module, and the declaration is its own or not private.
inline bool can_name( const Module_graph& modules, const Ast& ast, File_id from, Node_id decl )
{
    const File_id file = ast.span( decl ).file;
    return modules.sees( from, file ) && ( from == file || ast.access( decl ) != Access::Private );
}

// Which declaration each use refers to, indexed by the use's Node_id. A side table rather than a
// field on Node: aux still holds the Symbol_id that diagnostics need, and the AST stays immutable
// after parsing.
// What nothing names, warned only when the front end found no error.
struct Unused
{
    std::vector<Node_id>                     names;    // parameters and locals
    std::vector<Node_id>                     walks;    // those of names that are a walk's lone binding
    std::vector<Node_id>                     privates; // top-level private declarations nothing public reaches
    std::vector<std::pair<File_id, File_id>> imports;  // (importer, imported) where the importer names nothing imported
};

class Resolution
{
public:
    Resolution() = default;
    Resolution(
        std::vector<Node_id> bindings,
        std::vector<bool>    unresolved,
        std::vector<bool>    unreported,
        std::vector<Node_id> next_overload,
        std::vector<Node_id> fallbacks,
        const Prelude_items& prelude,
        const Module_graph&  modules,
        Unused               unused = {}
    )
        : bindings_( std::move( bindings ) ),
          unresolved_( std::move( unresolved ) ),
          unreported_( std::move( unreported ) ),
          next_overload_( std::move( next_overload ) ),
          fallbacks_( std::move( fallbacks ) ),
          prelude_( prelude ),
          modules_( modules ),
          unused_( std::move( unused ) )
    {
    }

    // Invalid when the use was never resolved - an unknown name, or a node that is not a use. With
    // overloading this is the *first* candidate: the rest follow it through next_overload.
    Node_id declaration_of( Node_id use ) const
    {
        return use.v < bindings_.size() ? bindings_[use.v] : Node_id {};
    }

    // D45: the prelude's set a function's own set falls back to, keyed by the own set's first
    // declaration; invalid where there is none. Only an unqualified call may ask.
    Node_id fallback_of( Node_id decl ) const
    {
        return decl.v < fallbacks_.size() ? fallbacks_[decl.v] : Node_id {};
    }

    // The function a local or parameter of its name hides; invalid where none is.
    Node_id hidden_function( Node_id local ) const
    {
        return fallback_of( local );
    }

    // The next declaration of the same name in the same scope, or invalid at the end of the chain.
    // Per-declaration rather than per-use: a use has one name, and the set belongs to the name.
    Node_id next_overload( Node_id declaration ) const
    {
        return declaration.v < next_overload_.size() ? next_overload_[declaration.v] : Node_id {};
    }

    bool sees( const Ast& ast, File_id from, Node_id decl ) const
    {
        return can_name( modules_, ast, from, decl );
    }

    Span import_span( File_id from, File_id to ) const
    {
        return modules_.import_span( from, to );
    }

    std::vector<File_id> common_importers( File_id a, File_id b ) const
    {
        return modules_.common_importers( a, b );
    }

    Symbol_id package_of( File_id file ) const
    {
        return modules_.package_of( file );
    }

    bool is_package( Symbol_id package ) const
    {
        return modules_.is_package( package );
    }

    Symbol_id prelude_package() const
    {
        return modules_.prelude_package();
    }

    const Prelude_items& prelude() const
    {
        return prelude_;
    }

    const Module_graph& modules() const
    {
        return modules_;
    }

    // A qualified name the resolver could not bind, and has already answered for.
    bool is_unresolved( Node_id use ) const
    {
        return use.v < unresolved_.size() ? unresolved_[use.v] : false;
    }

    // An undeclared `case` label name left for the checker, which knows the scrutinee's variants.
    bool is_unreported( Node_id use ) const
    {
        return use.v < unreported_.size() ? unreported_[use.v] : false;
    }

    const Unused& unused() const
    {
        return unused_;
    }

private:
    std::vector<Node_id> bindings_;
    std::vector<bool>    unresolved_;
    std::vector<bool>    unreported_;
    std::vector<Node_id> next_overload_;
    std::vector<Node_id> fallbacks_;
    Prelude_items        prelude_;
    Module_graph         modules_;
    Unused               unused_;
};

// Whether an `implements ... for` block covers every instance of its type: each argument a distinct
// parameter of the block, and no bound but `copyable`. In the type's own package that is refused.
bool covers_every_instance( const Ast& ast, const Interner& interner, Node_id block );

// A loaded module's bindings are taken from its state rather than resolved again.
Resolution resolve(
    const Ast&                    ast,
    const Source_manager&         sm,
    const Interner&               interner,
    Diagnostics&                  diags,
    const Module_graph&           modules = {},
    std::span<const Loaded_state> states  = {}
);

} // namespace keel
