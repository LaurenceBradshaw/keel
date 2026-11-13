// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <string_view>
#include <variant>
#include <vector>
#include "common/literal_pool.h"
#include "common/span.h"
#include "egraph/circuit.h"
#include "egraph/field.h"
#include "ir/kir.h"
#include "ir/purity.h"
#include "sema/type.h"

namespace keel
{
// A field of a struct or an element of an array.
struct Step
{
    bool    index = false;
    u64     element {}; // index only
    Node_id field {};
};

// Parameter `local`, or what it points to when `through`, then `steps`.
struct Param_path
{
    Local_id          local {};
    bool              through = false;
    std::vector<Step> steps;
};

// `imaginary` only when the map was paired into complex rows.
struct Value_path
{
    Param_path real;
    Param_path imaginary;
};

struct Recognised
{
    Field                   field;
    std::vector<Row>        rows; // one per output, one element per input
    std::vector<Value_path> inputs;
    std::vector<Value_path> outputs;
    Type_id                 type;  // f64, or the integer type of the ring
    Cost                    input; // the adds and muls the program ran
};

enum class Ending : u8
{
    Product,     // two inputs multiplied
    Compare,     // an input compared
    Branch,      // a branch on a value known only at run time
    Escape,      // an input leaving other than through an output
    Effect,      // a global written, or a call with effects
    Shift,       // an integer input shifted
    Division,    // an integer input divided, or anything divided by an input
    Remainder,   // an integer input's remainder
    Bits,        // an input's bits
    Cast,        // an input cast to another type
    Coefficient, // an input scaled by a value with no exact value
    Constant,    // a constant added to an input, or written to an output
    Unknown,     // a value known only at run time written to an output
    Global,      // a global read
    Panic,       // a path that panics or is unreachable
    Unsupported, // an operation it does not follow
    Mixed,       // inputs of two types
    Empty,       // no input or no output
    Budget,      // the step budget spent
};

struct Unrecognised
{
    Ending why;
    Span   span;
};

// What ended an attempt, as "multiplies two inputs".
std::string_view describe( Ending why );

// Runs `func` with its inputs as symbols: by-value number parameters, and what pointer parameters
// reach. Integers, addresses and branches are concrete; numbers are linear forms over the inputs
// with exact coefficients in Q(W_M), or Z/2^w for an integer ring. Outputs are what the run writes
// through pointer parameters, and the return. Pure callees run too; a `@trig_*` call at a rational
// multiple of pi is exact. A real 2N map of rotation blocks is paired into N complex rows.
std::variant<Recognised, Unrecognised> recognise(
    const Function&              func,
    const std::vector<Function>& functions,
    const Purity&                purity,
    const Type_table&            types,
    const Literal_pool&          literals,
    u64                          budget = 4'000'000
);
} // namespace keel
