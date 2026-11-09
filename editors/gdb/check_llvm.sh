#!/usr/bin/env bash
# Copyright 2026 Laurence Bradshaw
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# Builds examples/debugging.kl with the LLVM backend and --debug, and checks gdb names functions,
# locals and fields as Keel does with no Python helper loaded. Not in pre-commit, which should not
# need gdb.

set -euo pipefail

cd "$( dirname "$0" )/../.."

./build/debug/bin/keelc --backend llvm --debug --package kl=keel_stl/src examples/debugging.kl \
    -o build/examples/debugging_llvm

out="$( gdb -batch -nx -ex "break debugging.kl:16" -ex "break debugging.kl:39" -ex "break kl::print" -ex run -ex bt \
    -ex "p p" -ex continue -ex "info locals" -ex "p name.size_" -ex continue -ex "bt 1" \
    build/examples/debugging_llvm 2>&1 )"

status=0
for expected in \
    '#0  manhattan (p=...) at debugging.kl:16' \
    '#1  0x[0-9a-f]* in main () at debugging.kl:24' \
    '= {x = 3, y = -4}' \
    'name = {data_ = 0x[0-9a-f]* "keel", size_ = 4, cap_ = 4}' \
    'sum = 70' \
    '= 4$' \
    '#0  kl::print (text=..., end=...) at console.kl'; do
    grep -q -- "$expected" <<< "$out" || { echo "missing: $expected"; status=1; }
done

# Temporaries have no names to show.
if grep -qE '^t[0-9]+ = ' <<< "$out"; then
    echo "a temporary is listed"
    status=1
fi

# Into `str( … )`: past the prologue, so its parameters hold what was passed.
into="$( gdb -batch -nx -ex "break debugging.kl:25" -ex run -ex step build/examples/debugging_llvm 2>&1 )"
grep -q 'str::str (this=0x[0-9a-f]*, data=0x[0-9a-f]* "keel", size=4) at <prelude>:27' <<< "$into" ||
    { echo "step into str( … ) missing its arguments"; status=1; }

[ "$status" -eq 0 ] && echo "pass" || echo "$out"
exit "$status"
