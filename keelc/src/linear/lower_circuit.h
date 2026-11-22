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
#include "linear/sums.h"
#include "sema/type.h"

namespace keel
{
// Where one of a map's values lives; `imaginary` only in a complex or Gaussian ring.
struct Value_place
{
    Place real;
    Place imaginary;
};

// Makes the function computing a program's called kernel alone, by its program, once.
using Called = std::function<void( const Kernel&, const Loop_program& )>;

// That function's type arguments, its n, m, M and c as sizes and then `real` unless it is f64, and its
// parameter p's type: `const real[n]*` for the real and imaginary x, `real[m]*` for y's.
std::vector<Type_id> kernel_arguments( Type_table& types, const Kernel& k, Type_id real );
Type_id              kernel_parameter( Type_table& types, const Kernel& k, u32 p, Type_id real );

// Statements writing each output the value of its node in `nodes` (Circuit::k_zero for zero), from
// `inputs`, at `type`: float parts, or wrapping integers of the ring's width. Units fold into signs, so
// x + (-y) is one subtract. Every input is read before any output is written, so the two may share
// places. A debug build checks the statements against `rows` on random input, when given.
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
    const std::vector<Row>&         rows = {}
);

// `program` in place of `func`'s body, after `prologue`: loops over its leaves' statements, from x's arrays (buffer 0)
// to y's (buffer 1), each part a whole array of `real`. Temporaries are local arrays and tables read-only ones. A call
// passes the addresses of its first elements, real and imaginary swapped when conjugated.
void lower_program(
    Function&              func,
    const Loop_program&    program,
    Value_place            x,
    Value_place            y,
    std::vector<Statement> prologue,
    Type_id                real,
    Type_table&            types,
    Literal_pool&          literals,
    Span                   span,
    const Called&          called = {}
);
} // namespace keel
