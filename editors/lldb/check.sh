#!/usr/bin/env bash
# Copyright 2026 Laurence Bradshaw
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# Windows' editors/gdb/check.sh: builds examples/debugging.kl with --debug, and checks lldb reads
# the .pdb with functions, locals and fields named as Keel does. Run from Git Bash after the
# windows-debug preset is built. Not in pre-commit, which should not need a debugger.
#
# lldb 18's native PDB reader cannot step into a call, even in clang-cl's own C++ (it crashes or
# fails to set the return breakpoint), so the check steps over and checks no step-in.

set -euo pipefail

cd "$( dirname "$0" )/../.."

lldb="$( command -v lldb || echo "/c/Program Files/LLVM/bin/lldb.exe" )"
export LLDB_USE_NATIVE_PDB_READER=1

./build/windows-debug/bin/keelc.exe --debug --package kl=keel_stl/src examples/debugging.kl \
    -o build/examples/debugging.exe

# -F: lldb resolves a qualified name from a .pdb only as the full name.
out="$( "$lldb" -b -o "b debugging.kl:16" -o "b debugging.kl:39" -o "b -F kl::print" -o run -o bt \
    -o "p p" -o continue -o "frame variable" -o "p name.size_" -o continue -o "bt 1" \
    build/examples/debugging.exe 2>&1 )"

status=0
for expected in \
    'frame #0: 0x[0-9a-f]* debugging.exe`manhattan(p=Point @ 0x[0-9a-f]*) at debugging.kl:16' \
    'frame #1: 0x[0-9a-f]* debugging.exe`main at debugging.kl:24' \
    '^  x = 3$' \
    '^  y = -4$' \
    '(kl::string) name = (data_ = "keel.*, size_ = 4, cap_ = 4)' \
    '(long long) sum = 70' \
    '(unsigned long long) 4$' \
    'frame #0: 0x[0-9a-f]* debugging.exe`kl::print(text=0x[0-9a-f]*, end=0x[0-9a-f]*) at console.kl'; do
    grep -q -- "$expected" <<< "$out" || { echo "missing: $expected"; status=1; }
done

# Temporaries have no names to show.
if grep -qE '^\([^)]*\) t[0-9]+ = ' <<< "$out"; then
    echo "a temporary is listed"
    status=1
fi

# Over `manhattan( p )`: the next statement, with its result in `d`.
over="$( "$lldb" -b -o "b debugging.kl:24" -o run -o next -o "frame variable d" \
    build/examples/debugging.exe 2>&1 )"
{ grep -q 'debugging.exe`main at debugging.kl:25' <<< "$over" && grep -q '(int) d = 7' <<< "$over"; } ||
    { echo "step over manhattan( p ) missing line 25 or d"; status=1; }

[ "$status" -eq 0 ] && echo "pass" || echo "$out"
exit "$status"
