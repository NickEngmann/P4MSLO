#!/usr/bin/env bash
# Fast "heartbeat" test suite — target < 4 minutes total.
#
# Use this during iterative development. Catches core-functionality
# regressions (boot, SPI capture, button routing, gallery basics, heap)
# in under 4 min so you can iterate on a fix without waiting 20+ min
# per cycle. Reserve `run_all.sh` for pre-commit / pre-push runs.
#
# Preconditions:
#   - P4 flashed with current firmware
#   - /dev/ttyACM0 (or P4MSLO_TEST_PORT=/dev/ttyACMn)
#
# Each test gets a hard wall-clock timeout so a pyserial hang can't
# hold the suite hostage (see _lib.py drain() — uses select() directly
# to bypass pyserial's internal read-loop that occasionally blocks
# longer than its own timeout when the USB CDC endpoint misbehaves).
set -eu
cd "$(dirname "$0")"

# Force a clean boot + wipe PIMSLO SD state so the fast suite isn't
# contaminated by prior captures / encodes. Test 14 does a real
# photo_btn capture; leftover /sdcard/p4mslo_* content from earlier
# runs caused Store Access Fault panics in the P4 photo save path.
: "${P4MSLO_TEST_PORT:=/dev/ttyACM0}"
echo "Rebooting + wiping SD PIMSLO state on ${P4MSLO_TEST_PORT}..."
python3 -c "
import serial, time
s = serial.Serial('${P4MSLO_TEST_PORT}', 115200, timeout=0.3)
s.write(b'reboot\n'); s.flush(); time.sleep(0.5)
s.close()
"
sleep 10
python3 -c "
import serial, time
s = serial.Serial('${P4MSLO_TEST_PORT}', 115200, timeout=0.3)
for d in ['/sdcard/p4mslo', '/sdcard/p4mslo_gifs', '/sdcard/p4mslo_small',
          '/sdcard/p4mslo_previews']:
    s.write(('sd_rmrf ' + d + '\n').encode()); s.flush()
    time.sleep(4)
    resp = s.read(4000).decode('utf-8','replace').strip()
    for line in resp.splitlines():
        if 'sd_rmrf' in line and ('ok' in line or 'error' in line):
            print('  ' + line)
s.close()
"

TESTS=(
    01_boot_and_liveness.py
    12_dma_heap_health.py
    11_heartbeat.py
    14_capture_encode_offpage.py
)

# Per-test hard cap — `timeout` SIGTERMs the test if it runs longer.
# 180 s is more than any heartbeat test should ever need; getting
# near this means something broke.
PER_TEST_TIMEOUT=240

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
echo "  FAST SUITE PASSED: $pass / $total in ${elapsed}s"
echo "================================================================"
