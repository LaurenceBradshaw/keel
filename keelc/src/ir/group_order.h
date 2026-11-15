// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <functional>
#include <vector>
#include "ir/kir.h"

namespace keel
{
// The groups of `callees_first`, each optimised once, after every group it calls, when first wanted:
// in order, or earlier when a pass asks for one of its functions.
class Group_order
{
public:
    using Optimise = std::function<void( const std::vector<u32>& )>;

    // `also` adds edges the statements do not show.
    Group_order( const std::vector<Function>& functions, const std::vector<std::vector<u32>>& also, Optimise optimise );

    // `f`'s group optimised, and first what it calls; whether it is, as one under way is not.
    bool want( u32 f );

    // Every group, in order.
    void want_all();

    // Groups for the functions from `first` on, which none before calls.
    void add( u32 first );

private:
    enum class State : u8
    {
        Waiting,
        Optimising,
        Done,
    };

    void want_group( u32 g );

    const std::vector<Function>&         functions_;
    const std::vector<std::vector<u32>>& also_;
    Optimise                             optimise_;
    std::vector<std::vector<u32>>        groups_;
    std::vector<std::vector<u32>>        edges_;
    std::vector<u32>                     group_of_;
    std::vector<State>                   state_;
};
} // namespace keel
