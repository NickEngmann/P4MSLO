#!/usr/bin/env bash
# Runs the full host-side architecture validation suite.
#
# Usage:
#   tests/host_encode/run.sh           # build (if needed) + run all
#   tests/host_encode/run.sh --rebuild # force a clean rebuild first
#
# Exits non-zero on any failure.
set -eu
cd "$(dirname "$0")"

if [ "${1:-}" = "--rebuild" ] || [ ! -d build ]; then
    rm -rf build
    mkdir -p build
fi

cd build
[ -f Makefile ] || cmake .. > cmake.log
make -j$(nproc) > make.log

failures=0
run_test() {
    local name="$1" ; shift
    echo
    echo "================================================================"
    echo "  $name"
    echo "================================================================"
    if ! "./$name" "$@"; then
        failures=$((failures + 1))
        echo "  ✗ $name FAILED"
    fi
}

run_test host_encode "$(realpath ../../../debug_gifs)" /tmp/host_encode.gif --stress 5
run_test test_budget
run_test test_phases
run_test test_timing
run_test test_e2e

echo
echo "================================================================"
if [ "$failures" -eq 0 ]; then
    echo "  HOST SUITE: ALL PASS"
    exit 0
else
    echo "  HOST SUITE: $failures FAILED"
    exit 1
fi
