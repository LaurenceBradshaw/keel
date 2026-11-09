// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>
#include "ast/ast.h"
#include "ast/node.h"
#include "common/interner.h"
#include "lex/token.h"
#include "sema/reporter.h"
#include "sema/type.h"

namespace keel
{

// One `implements`: the implementer's method for each of the interface's, in the interface's order.
struct Conformance
{
    Node_id              implementer {}; // the aggregate or the block whose methods answer
    Type_id              type {};        // what conforms: an aggregate's open form, or a block's type
    bool                 block = false;  // `type` is a pattern, matched with the block's `where` met
    Type_id              interface {};
    std::vector<Node_id> requirements {}; // the interface's methods, in declaration order
    std::vector<Node_id> methods {};      // parallel to requirements; invalid where none answers
};

enum class Conformance_problem : u8
{
    Missing,     // no method of the name
    Mismatched,  // one of the name, with other parameters or another return
    Not_const,   // the interface's is `const` and the implementer's is not
    Static,      // the implementer's takes no object
    Not_static,  // the requirement takes no object, the implementer's does
    Generic,     // the implementer's has type parameters of its own
    Private,     // the implementer's is hidden from a caller holding the interface
    Repeated,    // the type already conforms to the interface
    Overlapping, // another block or `implements` could make one instance conform as well
    Unrequested, // a block's method the interface does not declare
    Unrefined,   // an implied interface a block answers only under a `where` this type does not meet
    Located,     // the implementer's receives a location the interface's callers cannot pass
    Defaulted,   // the implementer's has a default the interface's callers would not see
};

struct Conformance_error
{
    Conformance_problem problem = Conformance_problem::Missing;
    Span                at {};          // the interface as written; an unrequested method's name
    Node_id             implementer {}; // the aggregate or the block
    Type_id             interface {};
    Node_id             requirement {}; // the interface's method
    Node_id             found {};   // the implementer's method of that name, where one exists; Repeated: the first implementer
    Type_id             self {};    // the implementer's type, which the requirement's `self` stands for
    Type_id             refined {}; // Overlapping: the earlier conformance's type; Unrefined: the interface implied
};

// Everything `implements` means, filled in two phases. Phase 1, while signatures are declared:
// the relation as written, what each interface refines, and which interfaces are bounds only.
// Phase 2, `complete`, once every signature is typed: the method answering each requirement, and
// the errors in each conformance. Read by the body checks, lowering, and the emitter's vtables.
class Conformance_model
{
public:
    // Phase 1. `type` is an aggregate's open form; a generic's instances share it.
    void declare( const Type_table& table, Node_id implementer, Type_id type, Type_id interface );
    // A block's type is a pattern its parameters are bound by matching.
    void declare_block( Node_id block, Type_id pattern, Type_id interface );
    // What a type parameter promises, refinements included; a block's `where` is read from it.
    void declare_promised( Type_id parameter, std::span<const Type_id> interfaces );
    void declare_payload_enum( Node_id declaration ); // which enums the compiler gives no `==`
    // What an interface requires through its own `implements`, in written order.
    void declare_refinement( const Type_table& table, Type_id interface, Type_id refined );
    // The first method that is `static`, a walk, or mentions `self`, which makes the interface a bound only.
    void declare_bound_only( Node_id interface, Node_id method );
    // An interface whose requirement closing a cycle was refused, so its refinements are incomplete.
    void declare_cyclic( Node_id interface );

    // What `type` declares, at this instance: `last<i32>` implements `sink<i32>` through `last<T>`'s `sink<T>`.
    bool    declares( const Type_table& table, Type_id type, Type_id interface ) const;
    Node_id implementer_of( const Type_table& table, Type_id type, Type_id interface ) const; // the first, or invalid
    // supplied_by_kind, unless the type's own `implements` answers for it or for `equatable`.
    bool supplies( const Ast& ast, const Type_table& table, Type_id type, Type_id interface ) const;
    // What the compiler makes a type of this kind, whatever the type declares.
    static bool supplied_by_kind( const Ast& ast, const Type_table& table, Type_id type, Type_id interface );
    // The prelude interface an operator asks for: `equatable` for `==` and `!=`, `comparable` for the
    // orderings, and invalid otherwise.
    static Type_id interface_for( const Type_table& table, Token_kind op );
    // Whether `type` meets `interface`: declared, supplied, or promised by a parameter.
    bool conforms( const Type_table& table, Type_id type, Type_id interface ) const;
    // The bindings under which `block`'s pattern is `type`, its `where` met; nullopt where it is not.
    std::optional<Bindings> covers( const Type_table& table, Type_id pattern, Type_id type ) const;
    // A block whose type matches `type` for `interface` but whose `where` fails there: what it misses first.
    struct Unmet_bound
    {
        Node_id block;
        Type_id parameter;
        Type_id interface;
        Type_id argument;
    };
    std::optional<Unmet_bound> unmet_bound( const Type_table& table, Type_id type, Type_id interface ) const;
    // A conformance's parameters at `type`: a block's by matching, an aggregate's by its instance.
    std::optional<Bindings> bindings_at( const Type_table& table, const Conformance& conformance, Type_id type ) const;
    // Type_table::holds, with an implementer reaching its interface as a borrow, a pointer or a box.
    bool holds( const Type_table& table, Type_id from, Type_id to ) const;

    // Each instance of the Interface_decl `interface` that `type` conforms to: declared, a block's,
    // the compiler's, or a parameter's promise. Implied ones are declared, so they are included.
    std::vector<Type_id> instances_of( Type_table& table, Type_id type, Node_id interface ) const;

    std::vector<Type_id> refinements( Type_table& table, Type_id interface ) const; // substituted for this instance
    // Whether `other` is required by `interface`, directly or through what it requires.
    bool refines( Type_table& table, Type_id interface, Type_id other ) const;
    // The same, between Interface_decl nodes, whatever the arguments.
    bool    refines_declaration( const Type_table& table, Node_id interface, Node_id other ) const;
    Node_id bound_only_method( const Type_table& table, Type_id interface ) const; // invalid when it may be a type
    // Whether the interface, or one it requires, was declared cyclic.
    bool reaches_cyclic( const Type_table& table, Node_id interface ) const;

    // Phase 2: asked of each aggregate's `implements` list, and of each block.
    void complete( const Ast& ast, Type_table& table, std::span<const Type_id> recorded );

    std::span<const Conformance>       all() const;
    std::span<const Conformance_error> errors() const;
    const Conformance*                 find( Node_id implementer, Type_id interface ) const;
    // By the type that conforms, so `Box<i32>` finds `Box<T>`'s.
    const Conformance* of( const Type_table& table, Type_id type, Type_id interface ) const;
    Node_id            method_for( const Type_table& table, Type_id type, Type_id interface, Node_id requirement )
        const; // invalid where none answers
    // The `implements ... for type` block declaring `name`, or invalid.
    Node_id block_declaring( const Ast& ast, const Type_table& table, Type_id type, Symbol_id name ) const;
    // Every such block, in declaration order.
    std::vector<Node_id> blocks_declaring( const Ast& ast, const Type_table& table, Type_id type, Symbol_id name ) const;

private:
    struct Declared
    {
        Node_id implementer;
        Type_id interface; // as written: a generic implementer's names its own parameters
    };

    struct Block
    {
        Node_id node;
        Type_id pattern;
        Type_id interface; // as written, naming the block's parameters
    };

    bool kind_supplies( const Type_table& table, Type_id type, Type_id interface ) const;

    void conform(
        const Ast&               ast,
        Type_table&              table,
        std::span<const Type_id> recorded,
        Node_id                  implementer,
        Type_id                  interface,
        Span                     at,
        Type_id                  type
    );
    void conform_implied(
        const Ast&               ast,
        Type_table&              table,
        std::span<const Type_id> recorded,
        Node_id                  implementer,
        Type_id                  interface,
        Span                     at,
        Type_id                  type
    );
    void check_requested( const Ast& ast, Node_id block, Type_id interface, Type_id type );

    std::unordered_map<u32, std::vector<Declared>> declared_; // struct or enum declaration -> what it implements
    std::vector<Block>                             blocks_;   // in declaration order
    std::unordered_map<u32, std::vector<Type_id>>  promised_; // parameter type -> what it promises
    std::vector<Node_id>                           payload_enums_;
    std::unordered_map<u32, std::vector<Type_id>>  refinements_; // Interface_decl node -> what it requires, as written
    std::unordered_map<u32, Node_id>               bound_only_;  // Interface_decl node -> the method mentioning `self`
    std::vector<Node_id>                           cyclic_;      // Interface_decl nodes with a refused requirement

    std::vector<Conformance>       conformances_;
    std::vector<Conformance_error> errors_;
    bool                           completed_ = false;
};

} // namespace keel

namespace keel::sema
{

// One error per problem, each at the interface as written after `implements`.
void report_conformance_errors(
    const Ast&               ast,
    const Interner&          interner,
    Type_table&              table,
    std::span<const Type_id> recorded,
    const Conformance_model& model,
    Reporter&                reporter
);

} // namespace keel::sema
