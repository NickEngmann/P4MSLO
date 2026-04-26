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
# 20-90 s; 420 s covers the slowest legit test (02 with its 300 s
# encode-complete wait + photo_btn + gallery setup overhead).
PER_TEST_TIMEOUT=420

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
    15_format_then_photo.py
)

# Force a clean boot — 11+ tests of capture/encode churn leave the
# PSRAM heap and camera-buffer lifetimes in a state that has surfaced
# late-suite panics (Store access fault at 83 min uptime in test 09).
# Starting every full run from a fresh boot keeps each test running
# against the same baseline heap state. Fire-and-forget: the `reboot`
# serial command (app_serial_cmd.c) prints its ack and then
# esp_restart()s, so we just sleep long enough for the device to come
# back up before the first test opens the port.
: "${P4MSLO_TEST_PORT:=/dev/ttyACM0}"
echo "Requesting device reboot on ${P4MSLO_TEST_PORT} for clean baseline state..."
python3 -c "
import serial, time, sys
try:
    s = serial.Serial('${P4MSLO_TEST_PORT}', 115200, timeout=0.3)
    s.write(b'reboot\n'); s.flush()
    time.sleep(0.5)
    print(s.read(200).decode('utf-8','replace').strip())
    s.close()
except Exception as e:
    print(f'reboot request failed ({e}) — proceeding with existing state')
"
# ESP32-P4 boots in 1-2 s but LVGL + video stream init adds another
# ~2 s before the serial_cmd task is reading. 8 s is comfortable margin.
sleep 8

# Wipe accumulated PIMSLO content from SD BEFORE running the suite.
# A full gallery (40+ entries) fragments the DMA-internal pool enough
# to OOM the tjpgd 32 KB work buffer and drags PIMSLO encodes from
# 50 s → 130+ s — both cause test 02 to fail on encode-wait timeout
# and watchdog-starved IDLE0 during the encode. Starting each full
# run from an empty SD makes capture + encode timing deterministic.
# Keep /sdcard/esp32_p4_pic_save (normal P4 photo album) around; only
# nuke the PIMSLO-related dirs + previews.
echo "Wiping SD PIMSLO state..."
python3 -c "
import serial, time
s = serial.Serial('${P4MSLO_TEST_PORT}', 115200, timeout=0.3)
for d in ['/sdcard/p4mslo', '/sdcard/p4mslo_gifs', '/sdcard/p4mslo_small',
          '/sdcard/p4mslo_previews']:
    s.write(('sd_rmrf ' + d + '\n').encode()); s.flush()
    time.sleep(6)  # wipe can be slow on 40+ entries
    resp = s.read(4000).decode('utf-8','replace').strip()
    # Print the 'ok sd_rmrf ...' response line
    for line in resp.splitlines():
        if 'sd_rmrf' in line and ('ok' in line or 'error' in line):
            print('  ' + line)
s.close()
"

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
