#!/usr/bin/env bash
#
# The kl package's golden tests: each directory is one program using the package, built and run by
# keelc's golden runner, whose header says what a fixture holds.
#
#   run_tests.sh <path-to-keelc> [--update]

here="$( cd "$( dirname "$0" )" && pwd )"

# Its own artifacts directory, so a fixture here cannot share a build directory with one of keelc's.
KEEL_ARTIFACTS="${KEEL_ARTIFACTS:-${here}/../../build/test-artifacts/keel_stl}" \
    exec "${here}/../../keelc/test/run_tests.sh" "$@" --tests "$here"
