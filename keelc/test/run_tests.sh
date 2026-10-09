#!/usr/bin/env bash
#
# Golden-file tests for keelc (docs/PLAN.md §10).
#
# Each suite directory holds a FLAGS file naming the compiler flags for that suite - tests/lex/FLAGS
# is "--dump-tokens", tests/parse/FLAGS is "--dump-ast". A suite without one is an error rather than
# a silent default, so a new directory cannot quietly test the wrong thing.
#
# For every tests/<suite>/*.kl:
#     stdout   is compared against <name>.kl.expected
#     stderr   is compared against <name>.kl.stderr   (must be empty if the file is absent)
#     exit code is compared against <name>.kl.exit    (must be 0 if the file is absent)
#
# A directory holding a main.kl is one program: main.kl is its test, and its other .kl files are
# the modules it imports rather than tests of their own.
#
# A suite holding a RUN file goes one step further: keelc builds the program itself, linking the
# runtime, and it is executed, and the program's exit code is compared against <name>.kl.run (0 if
# the file is absent). Without this a golden diff can only say the emitted C is unchanged, never that it is
# correct - and the codegen fixtures are written to check their own answers, which is worth
# nothing if nothing runs them. Where <name>.kl.out exists, the program's stdout and stderr together
# must match it too. A fixture expected to exit 134 is one that aborts, and is never run under
# valgrind: an abort leaves memory reachable on purpose, and valgrind reports the signal.
#
#   CC          the C compiler keelc runs         (default: cc)
#   KEEL_RUN_TIMEOUT  seconds a fixture may run   (default: 10)
#   KEEL_JOBS   fixtures checked at once          (default: nproc)
#   KEEL_VALGRIND=1   run each fixture under valgrind too, failing on any error it reports.
#                     M3's acceptance is that a destructor frees exactly once, which a golden
#                     exit code cannot see - a double free or a leak both still exit 0.
#   KEEL_CFLAGS flags for it, passed as $CFLAGS   (default: -std=c11 -Wall -Wextra -Werror)
#   KEEL_RT     the runtime library, as --runtime (default: the one built beside keelc; an ASan
#               build needs one built without sanitizers)
#   KEEL_ARTIFACTS  where the .c and binaries go  (default: ../../build/test-artifacts)
#
#   run_tests.sh <path-to-keelc>            check
#   run_tests.sh <path-to-keelc> --update   rewrite every expectation from current behaviour
#   run_tests.sh <path-to-keelc> --tests <dir>   check another project's corpus, laid out as this one
#
# --update records what the compiler does, not what it should do. Read the diff before committing.

set -uo pipefail

if [ $# -lt 1 ]; then
    echo "usage: run_tests.sh <path-to-keelc> [--update]" >&2
    exit 2
fi

# Resolve before cd, since the caller's path is relative to their working directory.
keelc="$( cd "$( dirname "$1" )" && pwd )/$( basename "$1" )"
shift

update=0
tests="$( dirname "$0" )"
while [ $# -gt 0 ]; do
    case "$1" in
        -u | --update ) update=1 ;;
        --tests ) [ $# -ge 2 ] || { echo "run_tests.sh: --tests needs a directory" >&2; exit 2; }
                  tests="$2"; shift ;;
        * ) echo "run_tests.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
    shift
done

if [ ! -x "$keelc" ]; then
    echo "run_tests.sh: '$keelc' is not executable" >&2
    exit 2
fi

# Paths appear in diagnostics, so run from tests/ to keep them stable regardless of caller cwd.
cd "$tests" || exit 2

# WSL appends the Windows PATH, and the C linker probes every entry for itself. Over /mnt each probe
# is slow enough to make a link ten times slower, which is most of the suite's runtime.
PATH="$( tr ':' '\n' <<< "$PATH" | grep -v '^/mnt/' | paste -sd: )"

# The runtime from the same build as keelc, so the preset under test links its own. keelc is handed
# it with --runtime and does the linking, which is what a run fixture is testing.
runtime="${KEEL_RT:-$( dirname "$keelc" )/../keel_rt/src/libkeel_rt.a}"

if [ ! -f "$runtime" ]; then
    echo "run_tests.sh: no runtime library at '$runtime'; build keel_rt, or name one with KEEL_RT" >&2
    exit 2
fi

runtime="$( cd "$( dirname "$runtime" )" && pwd )/$( basename "$runtime" )"

# keelc runs the C compiler; the runner only configures it.
cc="${CC:-cc}"
# -Werror, because a warning in emitted C is the compiler saying the code means something other
# than intended - that is the whole reason for building it here. The unused-* family included: a
# name the program never reads is marked used in the C, and `codegen/unused_names` checks it.
cflags="${KEEL_CFLAGS:--std=c11 -Wall -Wextra -Werror}"

# Deliberately outside tests/: the stray-file check below treats anything in a suite directory as a
# bug, and that check is worth more than the convenience of building in place.
artifacts="${KEEL_ARTIFACTS:-../../build/test-artifacts}"
run_timeout="${KEEL_RUN_TIMEOUT:-10}"

# Off by default: it multiplies the suite's runtime, and most fixtures allocate nothing for it to
# check. Turned on it is what makes "frees exactly once" a checked claim rather than an inspected one.
valgrind_run="${KEEL_VALGRIND:-}"

# A fixture that aborts would otherwise leave a core file in the suite.
ulimit -c 0
mkdir -p "$artifacts" || exit 2

if [ -t 1 ]; then
    red=$'\033[31m'; green=$'\033[32m'; dim=$'\033[2m'; reset=$'\033[0m'
else
    red=''; green=''; dim=''; reset=''
fi

# NUL-delimited: unquoted $( ) would word-split on spaces and glob-expand any * in a name.
mapfile -d '' -t sources < <( find . -name '*.kl' -print0 | sort -z )

# A module of a program is any other .kl at or below the directory holding its main.kl, which is
# where a golden keeps a package of its own.
programs=()
for src in "${sources[@]}"; do
    dir="$( dirname "$src" )"
    module=0

    while [ "$dir" != "." ]; do
        if [ -f "$dir/main.kl" ] && [ "$src" != "$dir/main.kl" ]; then
            module=1
            break
        fi
        dir="$( dirname "$dir" )"
    done

    [ "$module" -eq 0 ] && programs+=( "$src" )
done
sources=( "${programs[@]}" )

if [ ${#sources[@]} -eq 0 ]; then
    echo "run_tests.sh: no .kl fixtures found" >&2
    exit 2
fi

# Checked up front, because a job cannot stop the suite.
for src in "${sources[@]}"; do
    if [ ! -f "$( dirname "$src" )/FLAGS" ]; then
        echo "run_tests.sh: $( dirname "${src#./}" ) has no FLAGS file" >&2
        exit 2
    fi
done

pass=0
fail=0
parallel_jobs="${KEEL_JOBS:-$( nproc )}"
results="$( mktemp -d )"
trap 'rm -rf "$results"' EXIT

# Compares one stream against its expectation, or rewrites it under --update.
# $1 actual file  $2 expected file  $3 label  -> echoes a diff and returns 1 on mismatch
check_stream()
{
    local actual="$1" expected="$2" label="$3"

    if [ "$update" -eq 1 ]; then
        if [ -s "$actual" ]; then
            cp "$actual" "$expected"
        else
            rm -f "$expected"
        fi
        return 0
    fi

    if [ -f "$expected" ]; then
        if ! diff -u "$expected" "$actual" > /dev/null; then
            echo "    ${label} differs:"
            diff -u "$expected" "$actual" | sed 's/^/      /'
            return 1
        fi
    elif [ -s "$actual" ]; then
        echo "    unexpected ${label}:"
        sed 's/^/      /' < "$actual"
        return 1
    fi

    return 0
}

# Has keelc build the program and runs it. Only suites with a RUN marker reach this: for the others
# stdout is a token or AST dump, and there is no program.
# $1 the fixture path  $2.. the suite's flags  -> echoes any problem, and leaves the .c behind
check_run()
{
    local src="$1"
    shift

    # A directory each, since keelc names the .c after the input and every program is a main.kl.
    local build_dir="${artifacts}/$( echo "${src%.kl}" | tr '/' '_' )"
    local stem="${build_dir}/program"
    local source_c="${build_dir}/$( basename "$src" ).c"
    local build_log="${build_dir}/build.log"
    local run_log="${build_dir}/run.log"

    # Emptied first, so a failed build cannot leave last run's program behind to be run.
    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    # The suite's flags without --emit-c, which would stop keelc before it builds anything.
    local build_flags=()
    for flag in "$@"; do
        [ "$flag" != "--emit-c" ] && build_flags+=( "$flag" )
    done

    if ! CC="$cc" CFLAGS="$cflags" "$keelc" "${build_flags[@]}" "$src" --runtime "$runtime" -o "$stem" \
        > "$build_log" 2>&1; then
        echo "    keelc did not build the program:"
        sed 's/^/      /' < "$build_log"
        echo "      kept at ${source_c}"
        return 1
    fi

    # The program's own output is not compared, only its exit code - but it must not leak into the
    # message the caller is capturing. Under timeout because a fixture is a real program and a
    # control-flow bug is an infinite loop: without this the suite hangs instead of failing, which
    # is the worse of the two by a distance.
    local vg_log="${build_dir}/vg.log"

    local expected_run=0
    [ -f "${src}.run" ] && expected_run="$( cat "${src}.run" )"

    if [ -n "$valgrind_run" ] && [ "$expected_run" -ne 134 ]; then
        # Its own log, and judged by that rather than by an exit code: a program killed by a signal
        # reports the signal, not --error-exitcode, so a segfault would otherwise slip through. With
        # -q the file stays empty unless valgrind has something to say.
        rm -f "$vg_log"
        timeout "$run_timeout" valgrind -q --leak-check=full --log-file="$vg_log" "$stem" > "$run_log" 2>&1
    else
        # The program's streams only: timeout's own word on a signal is not the program's output.
        timeout "$run_timeout" sh -c 'exec "$0" > "$1" 2>&1' "$stem" "$run_log" 2> "${build_dir}/timeout.log"
    fi

    local ran=$?

    if [ -n "$valgrind_run" ] && [ -s "$vg_log" ]; then
        echo "    valgrind reported an error:"
        sed 's/^/      /' < "$vg_log" | head -20
        echo "      kept at ${source_c}"
        return 1
    fi

    if [ "$ran" -eq 124 ]; then
        echo "    the program did not finish within ${run_timeout}s"
        echo "      kept at ${source_c}"
        return 1
    fi

    if [ "$update" -eq 1 ]; then
        [ -f "${src}.out" ] && cp "$run_log" "${src}.out"

        if [ "$ran" -ne 0 ]; then
            echo "$ran" > "${src}.run"
        else
            rm -f "${src}.run"
        fi

        return 0
    fi

    if [ "$ran" -ne "$expected_run" ]; then
        echo "    the program exited ${ran}, expected ${expected_run}"
        echo "      kept at ${source_c}"
        return 1
    fi

    if [ -f "${src}.out" ] && ! diff -u "${src}.out" "$run_log" > "${build_dir}/out.diff"; then
        echo "    the program's output differs:"
        sed 's/^/      /' < "${build_dir}/out.diff"
        echo "      kept at ${source_c}"
        return 1
    fi

    return 0
}

# Checks one fixture, and writes its verdict to $results/<index>: "pass", "FAIL" or "updated" on the
# first line, then any messages. $out and $err are its own, so fixtures can run side by side.
# $1 the index  $2 the fixture path
run_one()
{
    local index="$1" src="$2"
    local out="${results}/${index}.out" err="${results}/${index}.err"

    # Word splitting is wanted here, unlike for filenames: FLAGS holds one or more arguments.
    local suite_flags
    read -ra suite_flags < "$( dirname "$src" )/FLAGS"

    "$keelc" "${suite_flags[@]}" "$src" > "$out" 2> "$err"
    local code=$?

    local messages
    messages="$(
        check_stream "$out" "${src}.expected" "stdout"
        check_stream "$err" "${src}.stderr" "stderr"

        expected_code=0
        [ -f "${src}.exit" ] && expected_code="$( cat "${src}.exit" )"

        if [ "$update" -eq 1 ]; then
            if [ "$code" -ne 0 ]; then
                echo "$code" > "${src}.exit"
            else
                rm -f "${src}.exit"
            fi
        elif [ "$code" -ne "$expected_code" ]; then
            echo "    exit code: expected ${expected_code}, got ${code}"
        fi

        # Only when keelc succeeded: there is no C to build otherwise, and the failure above
        # already says so.
        if [ -f "$( dirname "$src" )/RUN" ] && [ "$code" -eq 0 ]; then
            check_run "$src" "${suite_flags[@]}"
        fi
    )"

    local verdict=pass
    if [ "$update" -eq 1 ]; then
        verdict=updated
    elif [ -n "$messages" ]; then
        verdict=FAIL
    fi

    { echo "$verdict"; [ -n "$messages" ] && echo "$messages"; } > "${results}/${index}"
}

running=0
for index in "${!sources[@]}"; do
    if [ "$running" -ge "$parallel_jobs" ]; then
        wait -n
        running=$(( running - 1 ))
    fi

    run_one "$index" "${sources[$index]#./}" &
    running=$(( running + 1 ))
done
wait

# In fixture order, whatever order they finished in.
for index in "${!sources[@]}"; do
    src="${sources[$index]#./}"
    { read -r verdict; messages="$( cat )"; } < "${results}/${index}"

    if [ "$verdict" = updated ]; then
        echo "  ${dim}updated${reset}  ${src}"
    elif [ "$verdict" = pass ]; then
        echo "  ${green}pass${reset}     ${src}"
        pass=$(( pass + 1 ))
    else
        echo "  ${red}FAIL${reset}     ${src}"
        echo "$messages"
        fail=$(( fail + 1 ))
    fi
done

if [ "$update" -eq 1 ]; then
    echo
    echo "expectations rewritten - review the diff before committing"
    exit 0
fi

# A suite whose FLAGS stop before emission must leave nothing behind. Nothing above would notice
# otherwise: a driver that wrongly compiles a --check fixture writes a .c file and an executable,
# and every stream comparison still passes because neither appears on stdout or stderr.
stray="$( find . -type f \
    ! -name '*.kl' ! -name '*.kl.expected' ! -name '*.kl.stderr' ! -name '*.kl.exit' \
    ! -name '*.kl.run' ! -name '*.kl.out' \
    ! -name FLAGS ! -name RUN ! -name run_tests.sh ! -name CMakeLists.txt | sort )"

if [ -n "$stray" ]; then
    echo
    echo "  ${red}FAIL${reset}     files left behind by the compiler:"
    echo "$stray" | sed 's/^/             /'
    fail=$(( fail + 1 ))
fi

echo
echo "${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ]
