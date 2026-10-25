// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "common/bytes.h"
#include "common/interner.h"
#include "common/literal_pool.h"
#include "common/source_manager.h"
#include "parse/loader.h"
#include "sema/type.h"

// A module's state names nodes, symbols, literals and types that another compile numbers
// differently. These two classes write each one in a form that does not depend on the numbering,
// and read it back into the new one.

namespace keel
{

// Writes references, and collects the tables they index: the modules (trees) the state names,
// entry 0 being the module itself, its symbols, and its types, each written after its parts.
class Ref_writer
{
public:
    Ref_writer(
        const Program&        program,
        const Type_table&     table,
        const Literal_pool&   literals,
        const Source_manager& sm,
        const Interner&       interner,
        File_id               module
    );

    void node( Byte_writer& out, Node_id id );  // in any tree
    void local( Byte_writer& out, Node_id id ); // in the module
    void symbol( Byte_writer& out, Symbol_id id );
    void literal( Byte_writer& out, Literal_id id );
    void type( Byte_writer& out, Type_id id );
    void span( Byte_writer& out, Span span ); // in any tree's file

    // The module's nodes, which run unbroken from first() to end().
    u32 first() const;
    u32 end() const;

    // The tables, once everything naming them has been written.
    void write_trees( Byte_writer& out ) const;
    void write_symbols( Byte_writer& out ) const;
    void write_types( Byte_writer& out ) const;

private:
    u32 tree( File_id file );
    u32 type_index( Type_id id );

    const Program&        program_;
    const Type_table&     table_;
    const Literal_pool&   literals_;
    const Source_manager& sm_;
    const Interner&       interner_;
    File_id               module_;
    u32                   first_ = 0;
    u32                   end_   = 0;

    std::vector<std::string>     trees_;
    std::unordered_map<u32, u32> tree_of_; // File_id -> index into trees_
    std::vector<Symbol_id>       symbols_;
    std::unordered_map<u32, u32> symbol_of_;
    std::string                  types_; // the entries, back to back
    u32                          type_count_ = 0;
    std::unordered_map<u32, u32> type_of_;
};

// Reads what Ref_writer wrote, into this compile's numbering. A reference that names nothing
// loaded fails the read, as a malformed one does.
class Ref_reader
{
public:
    Ref_reader(
        Byte_reader&          in,
        Program&              program,
        Interner&             interner,
        Literal_pool&         literals,
        Type_table&           table,
        const Source_manager& sm,
        File_id               file
    );

    // The tables, in the order the state holds them. Types name the module's own nodes, so they are
    // read after the tree.
    void read_trees();
    void read_symbols();
    void read_types();

    Node_id    node();
    Node_id    local();
    Symbol_id  symbol();
    Literal_id literal();
    Type_id    type();
    Span       span();

    bool failed() const;
    void fail(); // for a value the caller finds malformed

private:
    Byte_reader&          in_;
    Program&              program_;
    Interner&             interner_;
    Literal_pool&         literals_;
    Type_table&           table_;
    const Source_manager& sm_;
    File_id               file_;
    bool                  failed_ = false;

    std::vector<File_id>   trees_;
    std::vector<Symbol_id> symbols_;
    std::vector<Type_id>   types_;
};

} // namespace keel
