#!/usr/bin/env bash
# Full regression e2e suite — target total < 15 min. Each test gets a
# hard wall-clock timeout so a single pyserial hang or firmware wedge
# can't hold the suite hostage.
#
# Tests are ordered:
#   1. Cheap smoke first  (01, 12)           — fail fast on dead firmware
#   2. Core capture path  (10, 02, 13)       — the "does it work" tests
#   3. Capture edge cases (06, 08)
#   4. Gallery UX         (03, 04, 07, 09, 10)
#   5. Bg worker (slow)   (05)               — last, 150 s observation
#
# If you're iterating on a fix, use `run_fast.sh` instead (< 4 min
# heartbeat).
#
# Preconditions:
#   - P4 flashed with current firmware
#   - SPI cams (for tests that need captures) wired + powered
#   - /dev/ttyACM0 is the P4 (or set P4MSLO_TEST_PORT)
set -eu
cd "$(dirname "$0")"

# Per-test hard cap — `timeout` SIGTERMs on expiry. Most tests run in
# 20-90 s; 300 s covers the slowest legit test (05 with its 150 s
# bg-worker observation window + setup).
PER_TEST_TIMEOUT=300

TESTS=(
    01_boot_and_liveness.py
    12_dma_heap_health.py
    10_album_open_from_main.py
    02_camera_capture_to_gif.py
    13_spi_back_to_back.py
    06_capture_timing.py
    08_capture_edge_cases.py
    03_delete_modal.py
    07_gallery_edge_cases.py
    04_gallery_knob_nav.py
    09_gallery_empty_and_states.py
    05_bg_encode_while_on_gallery.py
)

total=${#TESTS[@]}
pass=0
t0=$(date +%s)
for t in "${TESTS[@]}"; do
    echo
    echo "================================================================"
    echo "  RUNNING: $t (hard cap ${PER_TEST_TIMEOUT}s)"
    echo "================================================================"
    if timeout --signal=TERM --kill-after=10 "$PER_TEST_TIMEOUT" python3 "$t"; then
        pass=$((pass + 1))
    else
        rc=$?
        echo
        if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
            echo "  $t TIMED OUT (> ${PER_TEST_TIMEOUT}s) — killed"
        else
            echo "  $t FAILED (rc=$rc) — stopping suite"
        fi
        echo "  Full log: $(dirname $0)/${t%.py}.log"
        exit 1
    fi
done

t1=$(date +%s)
elapsed=$((t1 - t0))
echo
echo "================================================================"
echo "  SUITE PASSED: $pass / $total in ${elapsed}s"
echo "================================================================"
