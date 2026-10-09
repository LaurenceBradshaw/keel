#!/usr/bin/env bash
#
# Golden-file tests for keeldoc: documents the package in geo/ and compares every page with the one
# in expected/. Run from anywhere:
#
#   run_tests.sh <path-to-keeldoc> <path-to-keelc>
#
# There is no --update: regenerate into a scratch directory, review the diff, then copy the pages.

set -uo pipefail

if [ $# -ne 2 ]; then
    echo "usage: run_tests.sh <path-to-keeldoc> <path-to-keelc>" >&2
    exit 2
fi

keeldoc="$( cd "$( dirname "$1" )" && pwd )/$( basename "$1" )"
keelc="$( cd "$( dirname "$2" )" && pwd )/$( basename "$2" )"

cd "$( dirname "$0" )" || exit 1

out="$( mktemp -d )"
trap 'rm -rf "$out"' EXIT

if ! "$keeldoc" --keelc "$keelc" -o "$out" geo=geo; then
    echo "keeldoc failed on geo/"
    exit 1
fi

if ! diff -ru expected "$out"; then
    echo "the pages differ from expected/ (above: - expected, + produced)"
    exit 1
fi

echo "keeldoc pages match ($( ls expected | wc -l ) files)"
