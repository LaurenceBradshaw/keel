// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "ast/ast.h"
#include "common/interner.h"
#include "lex/token.h"
#include "sema/reporter.h"
#include "sema/type_checker.h"

namespace keel::sema
{

// D78 (4): an array suffix over a size parameter, `u64[N - 1]`, can be 0 or overflow only at an
// instance. Walks the instances the written sizes reach and reports each bad one at the size the
// caller wrote. Runs only once D42 has passed, which is what makes the walk finish.
class Size_suffixes
{
public:
    Size_suffixes(
        const Ast& ast, const Interner& interner, Type_table& table, std::span<const Type_id> types, Reporter& reporter
    )
        : ast_( ast ),
          interner_( interner ),
          table_( table ),
          types_( types ),
          reporter_( reporter )
    {
    }

    // `roots` are edges out of non-generic code; `calls` and `implied` the generic graph.
    void
    check( std::span<const Generic_call> roots, std::span<const Generic_call> calls, std::span<const Generic_call> implied );

private:
    // Which written size a size came from: the `occurrence`th one of `value` in the origin's span.
    struct Source
    {
        u64 value      = 0;
        u32 occurrence = 0;
    };

    // A declaration and its size arguments; a type argument's slot is left invalid.
    struct Instance
    {
        Node_id                            declaration;
        std::vector<Type_id>               sizes;
        std::vector<std::optional<Source>> sources; // parallel to `sizes`
        std::vector<Type_id>               types;   // parallel; the type argument where `sizes` has none
    };

    struct Suffix
    {
        Node_id node;
        Type_id extent;
        bool    argument = false; // `bitset<(N - 1)>` rather than `u64[N - 1]`
    };

    // `where R >= C`, its sides as sizes.
    struct Condition
    {
        Node_id    node;
        Token_kind op;
        Type_id    lhs;
        Type_id    rhs;
    };

    void                    collect( Node_id id, Node_id owner, Node_kind parent = Node_kind::Error );
    std::optional<Instance> reach( const Generic_call& edge, const Bindings& bindings, const Instance* caller );
    Bindings                bindings_of( const Instance& instance ) const;
    void                    collect_conditions( Node_id declaration );
    bool                    report( const Instance& instance, Span origin );
    bool                    report_condition( const Instance& instance, Span origin, const Bindings& bindings );
    Span                    blame( const Instance& instance, const Bindings& bindings, Type_id size, Span origin ) const;
    std::string             spelled( Type_id size ) const;
    std::string             name_of( const Instance& instance ) const;
    Span                    written( Span origin, std::span<const u64> values, std::span<const Source> sources ) const;
    Type_id                 failing( Type_id size, const Bindings& bindings ) const;
    void                    parameters_of( Type_id size, std::vector<Type_id>& into ) const;

    const Ast&               ast_;
    const Interner&          interner_;
    Type_table&              table_;
    std::span<const Type_id> types_;
    Reporter&                reporter_;

    std::unordered_map<u32, std::vector<Suffix>>    suffixes_;   // by the generic that holds them
    std::unordered_map<u32, std::vector<Condition>> conditions_; // by the declaration they constrain
};

} // namespace keel::sema
