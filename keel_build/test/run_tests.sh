#!/usr/bin/env bash
#
# keelb's goldens. Each case is a folder holding `steps`, a bash script run in a copy of the folder
# with keelb on PATH, and `steps.out`, what it prints. Paths under the copy are printed relative to it.
# A folder named `.edits` is not a package, so it holds the files that steps copy in.
#
#   run_tests.sh <path-to-keelb> [--update]

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

pass=0
fail=0
for steps in "${here}"/*/steps; do
    case_dir="$( dirname "$steps" )"
    name="$( basename "$case_dir" )"

    rm -rf "${artifacts:?}/${name}"
    mkdir -p "$artifacts"
    cp -r "$case_dir" "${artifacts}/${name}"
    work="$( cd "${artifacts}/${name}" && pwd -P )"

    ( cd "$work" && PATH="${bin}:${PATH}" bash steps < /dev/null 2>&1 ) | sed "s#${work}/##g" > "${work}/steps.log"

    if [ "$update" -eq 1 ]; then
        cp "${work}/steps.log" "${case_dir}/steps.out"
        echo "  ${dim}updated${reset}  ${name}"
    elif diff -u "${case_dir}/steps.out" "${work}/steps.log" > "${work}/steps.diff" 2>&1; then
        echo "  ${green}pass${reset}     ${name}"
        pass=$(( pass + 1 ))
    else
        echo "  ${red}FAIL${reset}     ${name}"
        sed 's/^/      /' < "${work}/steps.diff"
        fail=$(( fail + 1 ))
    fi
done

[ "$update" -eq 1 ] && exit 0

echo
echo "${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
