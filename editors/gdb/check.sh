#!/usr/bin/env bash
# Copyright 2026 Laurence Bradshaw
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# Builds examples/debugging.kl, stops it at line 39 under gdb with keel.py loaded, and checks what
# the script shows, including a full path in each frame, which VS Code needs to open the file.
# Needs the debug preset's keelc and a gdb with Python. Not in pre-commit, which should not need gdb.

set -euo pipefail

cd "$( dirname "$0" )/../.."

CFLAGS=-g ./build/debug/bin/keelc --debug --package kl=keel_stl/src examples/debugging.kl -o build/examples/debugging

out="$( gdb -batch -nx -ex "source editors/gdb/keel.py" -ex "break debugging.kl:16" -ex "break debugging.kl:25" \
    -ex "break debugging.kl:39" -ex run -ex bt -ex continue -ex step -ex "bt 1" -ex continue -ex "bt full" \
    -ex "p kl_name_6" -ex "p kl_p_1" build/examples/debugging 2>&1 )"

status=0
for expected in \
    '#0  manhattan (kl_p_1=' \
    '#1  0x[0-9a-f]* in main () at /.*/examples/debugging.kl:24' \
    '#0  str (.*) at /.*/keelc/src/prelude/prelude.kl:32' \
    'kl_name_6 = "keel"' \
    'kl_xs_14 = kl::list<i32> (count 4) = {7, 14, 21, 28}' \
    'kl_sum_24 = 70' \
    '= {x = 3, y = -4}'; do
    grep -q -- "$expected" <<< "$out" || { echo "missing: $expected"; status=1; }
done

# Temporaries are hidden.
if grep -q 'kl_t[0-9]* = ' <<< "$out"; then
    echo "a temporary is listed"
    status=1
fi

[ "$status" -eq 0 ] && echo "pass" || echo "$out"
exit "$status"
