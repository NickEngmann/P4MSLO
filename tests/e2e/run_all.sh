#!/usr/bin/env bash
# Run the full on-device e2e suite sequentially. Each test is isolated —
# any non-PASS exit short-circuits.
#
# Preconditions:
#   - P4 is flashed with the current firmware
#   - SPI cams (for tests that need captures) are wired + powered
#   - /dev/ttyACM0 is the P4
#
# Usage:
#   tests/e2e/run_all.sh
set -eu
cd "$(dirname "$0")"

TESTS=(
    01_boot_and_liveness.py
    02_camera_capture_to_gif.py
    06_capture_timing.py
    08_capture_edge_cases.py
    03_delete_modal.py
    07_gallery_edge_cases.py
    04_gallery_knob_nav.py
    05_bg_encode_while_on_gallery.py
)

total=${#TESTS[@]}
pass=0
for t in "${TESTS[@]}"; do
    echo
    echo "================================================================"
    echo "  RUNNING: $t"
    echo "================================================================"
    if python3 "$t"; then
        pass=$((pass + 1))
    else
        echo
        echo "  $t FAILED — stopping suite"
        echo "  Full log: $(dirname $0)/${t%.py}.log"
        exit 1
    fi
done

echo
echo "================================================================"
echo "  SUITE PASSED: $pass / $total"
echo "================================================================"
