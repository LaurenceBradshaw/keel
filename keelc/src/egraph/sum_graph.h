// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/types.h"
#include "egraph/circuit.h"
#include "egraph/field.h"

namespace keel
{
// Input: `k` is the input. Add: classes `a <= b`. Scale: class `a` times element `k`. `input` marks
// the input program's nodes.
struct Graph_node
{
    Op_kind kind;
    bool    input;
    u32     k;
    u32     a;
    u32     b;
    Cost    cost;
};

// Every way found to build each output and its sub-sums: a class per linear form, its nodes the ways
// to make it. Shared parts of two forms (inputs grouped by coefficient ratio), butterflies
// p = (p + lq)/2 + (p - lq)/2 for a root of unity l, containment, and a first-input fallback.
class Sum_graph
{
public:
    // Rows are nonzero, at most 64 of them over at most 64 inputs. A fuel of 0 is unlimited.
    Sum_graph( Field& field, const std::vector<Row>& rows, u64 fuel );

    Field& field() const
    {
        return *field_;
    }

    u32 classes() const
    {
        return static_cast<u32>( forms_.size() );
    }

    const std::vector<Graph_node>& nodes( u32 c ) const
    {
        return nodes_[c];
    }

    // Classes equal up to a free unit share an orbit: form = unit * the orbit's representative.
    u32 orbit( u32 c ) const
    {
        return orbit_[c];
    }

    Element unit( u32 c ) const
    {
        return unit_[c];
    }

    // The class of each row.
    const std::vector<u32>& roots() const
    {
        return roots_;
    }

    // Work done, in the units of fuel.
    u64 spent() const;

private:
    using Form = std::array<Element, 64>;

    struct Form_hash
    {
        u32    n;
        size_t operator()( const Form& f ) const;
    };

    struct Node_key
    {
        u32     c, k, a, b;
        Op_kind kind;

        bool operator==( const Node_key& ) const = default;
    };

    struct Node_key_hash
    {
        size_t operator()( const Node_key& x ) const;
    };

    enum class Why : u8
    {
        Input,
        Shared,
        Contained,
        Fallback,
    };

    u32              get( const Form& f );
    void             add_node( u32 c, Graph_node n );
    void             split( u32 c, u64 mask, Why why );
    void             activate( u32 c );
    void             butterfly( u32 p, u32 q, u64 both, Element l, const std::vector<u32>& refs, std::vector<u32>& least );
    std::vector<u32> references( u32 p, u32 q, u64 both );
    u32              groups( const Form& f, const Form* r = nullptr );
    void             contain();
    void             finish();
    Cost             scale_cost( Element a );
    Element          lead( const Form& f, u64 s ) const;
    Form             scaled( Form f, Element c );
    Form             restricted( Form f, u64 mask ) const;
    u64              support( const Form& f ) const;
    bool             burnt();

    Field*               field_;
    u32                  n_;
    bool                 real_;
    bool                 halves_; // 1/2 exists, so butterflies can be made
    u64                  fuel_;
    u64                  base_;
    u64                  steps_       = 0;
    bool                 out_of_fuel_ = false;
    Element              half_;
    std::vector<Element> roots_of_unity_;

    std::vector<Form>                                             forms_;
    std::vector<u64>                                              supp_;
    std::vector<std::vector<Graph_node>>                          nodes_;
    std::unordered_map<Form, u32, Form_hash>                      ids_;
    std::unordered_set<Node_key, Node_key_hash>                   node_set_;
    std::unordered_map<u32, std::vector<std::pair<u32, Element>>> family_; // normalised class -> (class, scalar)
    std::vector<u32>                                              active_;
    std::vector<bool>                                             is_active_;
    std::vector<bool>                                             is_output_;
    std::vector<u32>                                              outputs_;
    std::vector<u32>                                              roots_;
    std::vector<u32>                                              orbit_;
    std::vector<Element>                                          unit_;
};
} // namespace keel
