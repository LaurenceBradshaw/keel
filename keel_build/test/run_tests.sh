#!/usr/bin/env bash
#
# keelb's goldens. Each case is a folder holding `steps`, a bash script run in a copy of the folder
# with keelb on PATH, and `steps.out`, what it prints. Paths under the copy are printed relative to it.
# A folder named `.edits` is not a package, so it holds the files that steps copy in.
#
#   run_tests.sh <path-to-keelb> [--update]
#
#   KEEL_COMPARE  a backend to build a second keelb with, from keelc beside the first; every case
#                 runs with both and must print its steps.out

set -uo pipefail

here="$( cd "$( dirname "$0" )" && pwd )"
artifacts="${KEEL_ARTIFACTS:-${here}/../../build/test-artifacts/keel_build}"
[ -x "${1:-}" ] || { echo "usage: run_tests.sh <path-to-keelb> [--update]"; exit 2; }
bin="$( cd "$( dirname "$1" )" && pwd )"
update=0
[ "${2:-}" = "--update" ] && update=1

if [ -t 1 ]; then
    red=$'\033[31m'; green=$'\033[32m'; dim=$'\033[2m'; reset=$'\033[0m'
else
    red=''; green=''; dim=''; reset=''
fi

# Each keelb's directory, its label, and where its cases are copied, under their own names since
# keelb names a program after its folder. keelb finds the keelc beside it.
bins=( "$bin" )
suffixes=( "" )
roots=( "$artifacts" )
if [ -n "${KEEL_COMPARE:-}" ] && [ "$update" -eq 0 ]; then
    second="${artifacts}/keelb_compare"
    rm -rf "$second"
    mkdir -p "$second"
    ln -s "${bin}/keelc" "${second}/keelc"
    if ! "${bin}/keelc" --quiet --backend "$KEEL_COMPARE" "${here}/../src/main.kl" -o "${second}/keelb"; then
        echo "  ${red}FAIL${reset}     keelc did not build keelb with --backend ${KEEL_COMPARE}"
        exit 1
    fi
    bins+=( "$second" )
    suffixes+=( " (keelb built with --backend ${KEEL_COMPARE})" )
    roots+=( "${artifacts}/compare" )
fi

pass=0
fail=0
for which in "${!bins[@]}"; do
    bin="${bins[$which]}"
    root="${roots[$which]}"
    for steps in "${here}"/*/steps; do
        case_dir="$( dirname "$steps" )"
        name="$( basename "$case_dir" )"
        label="${name}${suffixes[$which]}"

        rm -rf "${root:?}/${name}"
        mkdir -p "$root"
        cp -r "$case_dir" "${root}/${name}"
        work="$( cd "${root}/${name}" && pwd -P )"

        ( cd "$work" && PATH="${bin}:${PATH}" bash steps < /dev/null 2>&1 ) | sed "s#${work}/##g" > "${work}/steps.log"

        if [ "$update" -eq 1 ]; then
            cp "${work}/steps.log" "${case_dir}/steps.out"
            echo "  ${dim}updated${reset}  ${label}"
        elif diff -u "${case_dir}/steps.out" "${work}/steps.log" > "${work}/steps.diff" 2>&1; then
            echo "  ${green}pass${reset}     ${label}"
            pass=$(( pass + 1 ))
        else
            echo "  ${red}FAIL${reset}     ${label}"
            sed 's/^/      /' < "${work}/steps.diff"
            fail=$(( fail + 1 ))
        fi
    done
done

[ "$update" -eq 1 ] && exit 0

echo
echo "${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
