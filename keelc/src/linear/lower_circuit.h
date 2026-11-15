// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <array>
#include <functional>
#include <vector>
#include "common/literal_pool.h"
#include "common/span.h"
#include "ir/kir.h"
#include "linear/circuit.h"
#include "linear/field.h"

namespace keel
{
// Where one of a map's values lives; `imaginary` only in a complex or Gaussian ring.
struct Value_place
{
    Place real;
    Place imaginary;
};

// What a circuit's call of `callee` names: the function, its four `f64[N]` pointer parameters' types,
// `f64[N]`, `u64` and `void`.
struct Callee
{
    Node_id                declaration;
    std::vector<Type_id>   type_arguments;
    std::array<Type_id, 4> parameters;
    Type_id                array;
    Type_id                index;
    Type_id                none;
};
using Callees = std::function<Callee( u32 )>;

// Statements writing each output the value of its node in `nodes` (Circuit::k_zero for zero), from
// `inputs`, at `type`: f64 parts, or wrapping integers of the ring's width. Units fold into signs, so
// x + (-y) is one subtract. Every input is read before any output is written, so the two may share
// places. A call fills local arrays, calls through `callees`, and reads back what it wrote. A debug
// build checks the statements against `rows` on random input, when given.
std::vector<Statement> lower_circuit(
    Function&                       func,
    const Circuit&                  circuit,
    const Field&                    field,
    const std::vector<u32>&         nodes,
    const std::vector<Value_place>& inputs,
    const std::vector<Value_place>& outputs,
    Type_id                         type,
    Literal_pool&                   literals,
    Span                            span,
    const std::vector<Row>&         rows    = {},
    const Callees&                  callees = {}
);
} // namespace keel
