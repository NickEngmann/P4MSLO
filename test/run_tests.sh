#!/bin/bash
# P4MSLO Test Runner — builds and runs all host-based tests
# Usage: ./test/run_tests.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "===== P4MSLO Host-Based Test Suite ====="
echo ""

# Clean and build
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${SCRIPT_DIR}" -DCMAKE_BUILD_TYPE=Debug 2>&1
make -j$(nproc) 2>&1

echo ""
echo "===== Running Tests ====="

TOTAL=0
PASSED=0
FAILED=0
FAILED_TESTS=""

for test_bin in test_*; do
    if [ -x "$test_bin" ]; then
        echo ""
        if ./"$test_bin"; then
            PASSED=$((PASSED + 1))
        else
            FAILED=$((FAILED + 1))
            FAILED_TESTS="${FAILED_TESTS} ${test_bin}"
        fi
        TOTAL=$((TOTAL + 1))
    fi
done

echo ""
echo "====================================="
echo "Test Suites: ${TOTAL} total, ${PASSED} passed, ${FAILED} failed"
if [ $FAILED -gt 0 ]; then
    echo "Failed:${FAILED_TESTS}"
    exit 1
else
    echo "All test suites passed!"
    exit 0
fi
