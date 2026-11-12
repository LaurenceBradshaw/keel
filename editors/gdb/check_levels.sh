#!/usr/bin/env bash
# Copyright 2026 Laurence Bradshaw
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# Builds each keelc/test/opt/ fixture with --debug at -O0 and at -O1, steps every statement of
# its own file under gdb, and checks -O1 stops at -O0's lines in order (some skipped) with the same
# named locals at each. Needs a gdb with Python. Usage: check_levels.sh [keelc [fixture.kl]]

set -euo pipefail

cd "$( dirname "$0" )/../.."

keelc="${1:-./build/debug/bin/keelc}"

if [ $# -lt 2 ]; then
    fixtures=( keelc/test/opt/*.kl keelc/test/opt/*/main.kl )
    printf '%s\0' "${fixtures[@]}" | xargs -0 -n 1 -P "$( nproc )" "$0" "$keelc" || exit 1
    echo "pass: ${#fixtures[@]} fixtures"
    exit 0
fi

src="$2"
out="$( mktemp -d )"
trap 'rm -rf "$out"' EXIT

for level in 0 1; do
    "$keelc" --quiet --debug -O"$level" "$src" -o "$out/$level"
    timeout 120 gdb -batch -nx -ex "python import sys; sys.path.insert( 0, 'editors/gdb' ); import levels" \
        -ex "python levels.trace( '$out/$level.trace' )" "$out/$level" > /dev/null 2>&1 ||
        { echo "$src: gdb failed at -O$level"; exit 1; }
done

[ -s "$out/0.trace" ] || { echo "$src: no stops at -O0"; exit 1; }
report="$( python3 editors/gdb/levels.py compare "$out/0.trace" "$out/1.trace" )" ||
    { printf '%s:\n%s\n' "$src" "$( sed 's/^/    /' <<< "$report" )"; exit 1; }
