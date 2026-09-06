#!/usr/bin/env bash
#
# PLAN §3.3 step 3. Compiles every codegen fixture through both backends - the AST emitter and the
# KIR one - runs both, and requires the exit codes to agree.
#
# This is a real equivalence check rather than a text diff: the two backends legitimately produce
# different C, and most fixtures are self-checking, so agreeing on the answer is what matters. A
# fixture that returns a computed value (fib, gcd) is compared against itself rather than zero, so
# the check does not depend on knowing what it should be.
#
#   compare_backends.sh <path-to-keelc>
#
#   CC                the C compiler                   (default: cc)
#   KEEL_CFLAGS       flags for it
#   KEEL_RUN_TIMEOUT  seconds a fixture may run        (default: 10)

set -uo pipefail

if [ $# -lt 1 ]; then
    echo "usage: compare_backends.sh <path-to-keelc>" >&2
    exit 2
fi

keelc="$( cd "$( dirname "$1" )" && pwd )/$( basename "$1" )"

if [ ! -x "$keelc" ]; then
    echo "compare_backends.sh: '$keelc' is not executable" >&2
    exit 2
fi

cd "$( dirname "$0" )" || exit 2

cc="${CC:-cc}"
cflags="${KEEL_CFLAGS:--std=c11 -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable}"
run_timeout="${KEEL_RUN_TIMEOUT:-10}"

artifacts="${KEEL_ARTIFACTS:-../build/backend-comparison}"
mkdir -p "$artifacts" || exit 2

if [ -t 1 ]; then
    red=$'\033[31m'; green=$'\033[32m'; reset=$'\033[0m'
else
    red=''; green=''; reset=''
fi

# Builds one fixture through one backend and echoes the program's exit code, or a word naming the
# stage that failed. Never echoes nothing, so a caller can always compare two answers.
build_and_run()
{
    local source="$1" flag="$2" stem="$3"

    if ! "$keelc" "$flag" "$source" > "${stem}.c" 2> "${stem}.keelc.log"; then
        echo "keelc-failed"
        return
    fi

    # Unquoted on purpose: cflags is a list of arguments, not one.
    if ! $cc $cflags -o "$stem" "${stem}.c" > "${stem}.cc.log" 2>&1; then
        echo "cc-failed"
        return
    fi

    timeout "$run_timeout" "$stem" > "${stem}.run.log" 2>&1
    local code=$?

    if [ "$code" -eq 124 ]; then
        echo "timed-out"
        return
    fi

    echo "$code"
}

pass=0
fail=0

mapfile -d '' -t sources < <( find codegen -name '*.kl' -print0 | sort -z )

for source in "${sources[@]}"; do
    name="$( basename "$source" .kl )"

    from_ast="$( build_and_run "$source" --emit-c     "${artifacts}/${name}.ast" )"
    from_kir="$( build_and_run "$source" --emit-c-from-kir "${artifacts}/${name}.kir" )"

    if [ "$from_ast" = "$from_kir" ]; then
        printf "  %spass%s     %-24s both exited %s\n" "$green" "$reset" "$name" "$from_ast"
        pass=$(( pass + 1 ))
    else
        printf "  %sFAIL%s     %-24s ast: %s   kir: %s\n" "$red" "$reset" "$name" "$from_ast" "$from_kir"
        printf "             kept at %s/%s.{ast,kir}.c\n" "$artifacts" "$name"
        fail=$(( fail + 1 ))
    fi
done

echo
echo "${pass} agreed, ${fail} differed"
[ "$fail" -eq 0 ]
