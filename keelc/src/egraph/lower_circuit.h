// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <vector>
#include "common/literal_pool.h"
#include "common/span.h"
#include "egraph/circuit.h"
#include "egraph/field.h"
#include "ir/kir.h"

namespace keel
{
// Where one of a map's values lives; `imaginary` only in a complex or Gaussian ring.
struct Value_place
{
    Place real;
    Place imaginary;
};

// Statements computing each of `rows` by `circuit` into its `outputs` entry, from `inputs`, at
// `type`: f64 parts, or wrapping integers of the ring's width. Units fold into signs, so x + (-y)
// is one subtract. Every input is read before any output is written, so the two may share places.
std::vector<Statement> lower_circuit(
    Function&                       func,
    const Circuit&                  circuit,
    const Field&                    field,
    const std::vector<Row>&         rows,
    const std::vector<Value_place>& inputs,
    const std::vector<Value_place>& outputs,
    Type_id                         type,
    Literal_pool&                   literals,
    Span                            span
);
} // namespace keel
