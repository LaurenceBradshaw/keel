#!/usr/bin/env bash
#
# The kl package's tests.
#
# golden/ holds kl::test's own dependencies - console, list, optional, os, string - as keelc goldens,
# so a break there is caught by something that does not run through kl::test. keelc's runner checks
# them; its header says what a fixture holds.
#
# Every other main.kl is a program: unit/ is the kl::test suite, programs/ the cases a test cannot
# hold - a panic, and kl::test's own report. Each is built with --debug and without, and run in its
# own directory. It must exit as its main.kl.run says (0 if absent), and print what its main.kl.out
# holds where there is one; main.kl.args holds its arguments, one per line.
#
#   run_tests.sh <path-to-keelc> [--update]
#
#   KEEL_VALGRIND=1   run each program under valgrind too, failing on any error it reports
#   KEEL_JOBS, KEEL_RUN_TIMEOUT, CC, KEEL_CFLAGS, KEEL_C_WARNINGS, KEEL_RT   as for keelc's runner

set -uo pipefail

here="$( cd "$( dirname "$0" )" && pwd )"
artifacts="${KEEL_ARTIFACTS:-${here}/../../build/test-artifacts/keel_stl}"

KEEL_ARTIFACTS="${artifacts}/golden" "${here}/../../keelc/test/run_tests.sh" "$@" --tests "${here}/golden"
golden=$?

keelc="$( cd "$( dirname "$1" )" && pwd )/$( basename "$1" )"
update=0
[ "${2:-}" = "--update" ] && update=1

runtime="${KEEL_RT:-$( dirname "$keelc" )/../keel_rt/src/libkeel_rt.a}"
runtime="$( cd "$( dirname "$runtime" )" && pwd )/$( basename "$runtime" )"
run_timeout="${KEEL_RUN_TIMEOUT:-10}"
export CC="${CC:-cc}" CFLAGS="${KEEL_CFLAGS:--std=c11}" KEEL_C_WARNINGS="${KEEL_C_WARNINGS:--Wall -Wextra -Wpointer-arith -Werror}"

# Paths appear in panics and reports, so they are relative to here.
cd "$here" || exit 2
ulimit -c 0
PATH="$( tr ':' '\n' <<< "$PATH" | grep -v '^/mnt/' | paste -sd: )"

if [ -t 1 ]; then
    red=$'\033[31m'; green=$'\033[32m'; dim=$'\033[2m'; reset=$'\033[0m'
else
    red=''; green=''; dim=''; reset=''
fi

mapfile -t programs < <( find . -path ./golden -prune -o -name main.kl -printf '%h\n' | sed 's#^\./##' | sort )

results="$( mktemp -d )"
trap 'rm -rf "$results"' EXIT

# Builds and runs one program in one mode, writing any problem to $results/<index>.
# $1 the index  $2 the program's directory  $3 debug or release
run_one()
{
    local index="$1" dir="$2" mode="$3"
    local build_dir="${artifacts}/$( tr '/' '_' <<< "$dir" )_${mode}"
    local src="${dir}/main.kl"
    local flags=()
    [ "$mode" = debug ] && flags=( --debug )

    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    {
        if ! "$keelc" "${flags[@]}" --package kl=../src "$src" --runtime "$runtime" -o "${build_dir}/program" \
            > "${build_dir}/build.log" 2>&1; then
            echo "    keelc did not build it:"
            sed 's/^/      /' < "${build_dir}/build.log"
            return
        fi

        local args=()
        [ -f "${src}.args" ] && mapfile -t args < "${src}.args"
        local expected=0
        [ -f "${src}.run" ] && expected="$( cat "${src}.run" )"

        local runner=()
        if [ -n "${KEEL_VALGRIND:-}" ] && [ "$expected" -ne 134 ]; then
            runner=( valgrind -q --leak-check=full --log-file="${build_dir}/vg.%p.log" )
        fi

        # The program's streams only: timeout's own word on a signal is not the program's output.
        ( cd "$build_dir" && exec timeout "$run_timeout" sh -c 'log="$1"; shift; exec "$@" > "$log" 2>&1' \
            sh run.log "${runner[@]}" ./program "${args[@]}" ) < /dev/null 2> "${build_dir}/timeout.log"
        local ran=$?

        if [ "$update" -eq 1 ]; then
            # The debug build's answer is recorded; release must then agree with it.
            if [ "$mode" = debug ]; then
                [ -f "${src}.out" ] && cp "${build_dir}/run.log" "${src}.out"
                if [ "$ran" -ne 0 ]; then echo "$ran" > "${src}.run"; else rm -f "${src}.run"; fi
            fi
            return
        fi

        if [ "$ran" -eq 124 ]; then
            echo "    it did not finish within ${run_timeout}s"
        elif [ "$ran" -ne "$expected" ]; then
            echo "    it exited ${ran}, expected ${expected}:"
            grep -v '^pass ' < "${build_dir}/run.log" | sed 's/^/      /'
        elif [ -f "${src}.out" ] && ! diff -u "${src}.out" "${build_dir}/run.log" > "${build_dir}/out.diff"; then
            echo "    its output differs:"
            sed 's/^/      /' < "${build_dir}/out.diff"
        fi

        if [ -n "${runner[*]}" ] && [ -n "$( cat "${build_dir}"/vg.*.log 2> /dev/null )" ]; then
            echo "    valgrind reported an error:"
            cat "${build_dir}"/vg.*.log | sed 's/^/      /' | head -20
        fi
    } > "${results}/${index}"
}

jobs=()
for dir in "${programs[@]}"; do
    for mode in debug release; do
        jobs+=( "${dir} ${mode}" )
    done
done

running=0
for index in "${!jobs[@]}"; do
    if [ "$running" -ge "${KEEL_JOBS:-$( nproc )}" ]; then
        wait -n
        running=$(( running - 1 ))
    fi
    run_one "$index" ${jobs[$index]} 2> /dev/null &
    running=$(( running + 1 ))
done
wait

pass=0
fail=0
for index in "${!jobs[@]}"; do
    if [ "$update" -eq 1 ]; then
        echo "  ${dim}updated${reset}  ${jobs[$index]}"
    elif [ -s "${results}/${index}" ]; then
        echo "  ${red}FAIL${reset}     ${jobs[$index]}"
        cat "${results}/${index}"
        fail=$(( fail + 1 ))
    else
        echo "  ${green}pass${reset}     ${jobs[$index]}"
        pass=$(( pass + 1 ))
    fi
done

[ "$update" -eq 1 ] && exit 0

echo
echo "${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ] && [ "$golden" -eq 0 ]
