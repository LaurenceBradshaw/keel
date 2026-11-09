// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "ast/node.h"
#include "codegen/mangle.h"
#include "common/interner.h"
#include "common/span.h"
#include "ir/kir.h"
#include "sema/type.h"

namespace keel
{

class Ast;
class Types;

enum class Declared : u8
{
    Other,
    Function,
    Method,
    Constructor,
    Destructor,
    Lambda,
    Enum, // also the enum's synthesised destructor
    Aggregate,
    Interface,
    Global,
    Field, // a payload field and a capture too
};

// One declaration, as a backend needs it.
struct Described
{
    Declared    kind = Declared::Other;
    std::string name {}; // a lambda's is its site, `0lambda<offset>`
    Span        span {};
    bool        is_extern   = false;
    bool        receiver    = false;
    bool        requirement = false;

    // A function's result as it travels, a borrow as its pointer; a global's or a field's declared type.
    Type_id                        type {};
    std::vector<Mangled_parameter> parameters {};      // declared types and call markers, the receiver first
    std::vector<Type_id>           passed {};          // each as it travels
    std::vector<Type_id>           type_parameters {}; // what an instance's type arguments bind

    // A member's aggregate or block, which names a static member and a block's method.
    Node_id     aggregate {};
    std::string aggregate_name {};
    u32         aggregate_arity = 0;
    Type_id     implementer {}; // a block's type
    Type_id     conformed {};   // the interface a block's method answers

    std::string          symbol {};       // a field's
    Node_id              destructor {};   // an aggregate's own `~`
    bool                 payload = false; // an enum carrying payloads, which is a struct
    std::vector<Node_id> members {};      // an interface's requirements
};

// A field or capture at one instance of its aggregate.
struct Held_field
{
    Node_id declaration {};
    Type_id type {};
    Node_id variant {}; // a payload field's variant, else invalid
};

// The program as every backend is told it: each declaration's symbol and parameters and each
// aggregate's fields, so no backend reads the AST.
class Description
{
public:
    const Described& operator[]( Node_id declaration ) const;

    // An aggregate's fields or a closure's captures, each at this instance.
    std::span<const Held_field> fields( Type_id aggregate ) const;
    const Held_field*           field_named( Type_id aggregate, std::string_view name ) const;

    // Whether a value of `type` owns something to drop.
    bool owns( const Type_table& table, Type_id type ) const;

    // A place's type, a field's at its aggregate's instance.
    Type_id type_of( const Type_table& table, const Function& function, const Place& place ) const;

    std::span<const Node_id> globals() const; // every global and non-generic static, as declared
    std::span<const Node_id> externs() const; // every `extern` function, as declared

private:
    struct Instance
    {
        std::vector<Held_field> fields {};
        bool                    owns = false;
    };

    std::unordered_map<u32, Described> declarations_;
    std::unordered_map<u32, Instance>  instances_;
    std::vector<Node_id>               globals_;
    std::vector<Node_id>               externs_;

    friend Description describe( const Ast& ast, Types& types, const Interner& interner );
};

// Every declaration, and every aggregate instance the table holds, interning what a field names.
Description describe( const Ast& ast, Types& types, const Interner& interner );

} // namespace keel
