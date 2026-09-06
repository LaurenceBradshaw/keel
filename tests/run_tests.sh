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
# A suite holding a RUN file goes one step further: stdout is treated as C, compiled with $CC, and
# executed, and the program's exit code is compared against <name>.kl.run (0 if the file is
# absent). Without this a golden diff can only say the emitted C is unchanged, never that it is
# correct - and the codegen fixtures are written to check their own answers, which is worth
# nothing if nothing runs them.
#
#   CC          the C compiler                    (default: cc)
#   KEEL_RUN_TIMEOUT  seconds a fixture may run   (default: 10)
#   KEEL_CFLAGS flags for it                      (default: -std=c11 -Wall -Wextra -Werror)
#   KEEL_ARTIFACTS  where the .c and binaries go  (default: ../build/test-artifacts)
#
#   run_tests.sh <path-to-keelc>            check
#   run_tests.sh <path-to-keelc> --update   rewrite every expectation from current behaviour
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
for arg in "$@"; do
    case "$arg" in
        -u | --update ) update=1 ;;
        * ) echo "run_tests.sh: unknown option '$arg'" >&2; exit 2 ;;
    esac
done

if [ ! -x "$keelc" ]; then
    echo "run_tests.sh: '$keelc' is not executable" >&2
    exit 2
fi

# Paths appear in diagnostics, so run from tests/ to keep them stable regardless of caller cwd.
cd "$( dirname "$0" )" || exit 2

cc="${CC:-cc}"
# -Werror, because a warning in emitted C is the compiler saying the code means something other
# than intended - that is the whole reason for building it here. The unused-* family is excluded:
# a local the Keel program never reads becomes a local the C program never reads, which is faithful
# emission rather than a fault, and no amount of it can change what the program computes.
cflags="${KEEL_CFLAGS:--std=c11 -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable}"

# Deliberately outside tests/: the stray-file check below treats anything in a suite directory as a
# bug, and that check is worth more than the convenience of building in place.
artifacts="${KEEL_ARTIFACTS:-../build/test-artifacts}"
run_timeout="${KEEL_RUN_TIMEOUT:-10}"
mkdir -p "$artifacts" || exit 2

if [ -t 1 ]; then
    red=$'\033[31m'; green=$'\033[32m'; dim=$'\033[2m'; reset=$'\033[0m'
else
    red=''; green=''; dim=''; reset=''
fi

# NUL-delimited: unquoted $( ) would word-split on spaces and glob-expand any * in a name.
mapfile -d '' -t sources < <( find . -name '*.kl' -print0 | sort -z )

if [ ${#sources[@]} -eq 0 ]; then
    echo "run_tests.sh: no .kl fixtures found" >&2
    exit 2
fi

pass=0
fail=0
out="$( mktemp )"
err="$( mktemp )"
trap 'rm -f "$out" "$err"' EXIT

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

# Builds and runs the C on stdout. Only suites with a RUN marker reach this: for the others stdout
# is a token or AST dump, and handing that to a C compiler would be nonsense.
# $1 the fixture path  -> echoes any problem, and leaves the .c behind when there is one
check_run()
{
    local src="$1"
    local stem="${artifacts}/$( echo "${src%.kl}" | tr '/' '_' )"
    local source_c="${stem}.c"
    local build_log="${stem}.cc.log"
    local run_log="${stem}.run.log"

    cp "$out" "$source_c"

    # Unquoted on purpose: cflags is a list of arguments, not one.
    if ! $cc $cflags -o "$stem" "$source_c" > "$build_log" 2>&1; then
        echo "    the emitted C did not compile:"
        sed 's/^/      /' < "$build_log"
        echo "      kept at ${source_c}"
        return 1
    fi

    # The program's own output is not compared, only its exit code - but it must not leak into the
    # message the caller is capturing. Under timeout because a fixture is a real program and a
    # control-flow bug is an infinite loop: without this the suite hangs instead of failing, which
    # is the worse of the two by a distance.
    timeout "$run_timeout" "$stem" > "$run_log" 2>&1
    local ran=$?

    if [ "$ran" -eq 124 ]; then
        echo "    the program did not finish within ${run_timeout}s"
        echo "      kept at ${source_c}"
        return 1
    fi

    local expected_run=0
    [ -f "${src}.run" ] && expected_run="$( cat "${src}.run" )"

    if [ "$update" -eq 1 ]; then
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

    return 0
}

for src in "${sources[@]}"; do
    src="${src#./}"
    suite_flags_file="$( dirname "$src" )/FLAGS"

    if [ ! -f "$suite_flags_file" ]; then
        echo "run_tests.sh: $( dirname "$src" ) has no FLAGS file" >&2
        exit 2
    fi

    # Word splitting is wanted here, unlike for filenames: FLAGS holds one or more arguments.
    read -ra suite_flags < "$suite_flags_file"

    "$keelc" "${suite_flags[@]}" "${src#./}" > "$out" 2> "$err"
    code=$?

    problems=0
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
            check_run "$src"
        fi
    )"

    [ -n "$messages" ] && problems=1

    if [ "$update" -eq 1 ]; then
        echo "  ${dim}updated${reset}  ${src}"
    elif [ "$problems" -eq 0 ]; then
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
    ! -name '*.kl.run' \
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
