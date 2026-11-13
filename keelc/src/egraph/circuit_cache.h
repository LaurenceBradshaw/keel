// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
#include <filesystem>
#include <optional>
#include <vector>
#include "egraph/circuit.h"
#include "egraph/field.h"
#include "egraph/solve.h"

namespace keel
{
// Solved circuits kept across runs in `<folder>/circuits`, one file each, keyed by the map, the
// solve's options and keelc's build id. Nothing is evicted.

// The circuit an earlier solve stored for this map, checked to compute it; none for a missing,
// unreadable or wrong entry.
std::optional<Circuit>
cached_circuit( const std::filesystem::path& folder, Field& field, const std::vector<Row>& rows, const Solve_options& options );

// Written whole beside its name, then renamed, so a reader never sees half of one. Failure is silent.
void cache_circuit(
    const std::filesystem::path& folder,
    const Field&                 field,
    const std::vector<Row>&      rows,
    const Solve_options&         options,
    const Circuit&               circuit
);
} // namespace keel
