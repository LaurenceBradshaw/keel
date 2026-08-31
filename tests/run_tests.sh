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

echo
echo "${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ]
