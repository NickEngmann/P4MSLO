#!/usr/bin/env python3
"""Camera/capture edge cases.

Guards against:
  - Opening camera app should NOT show "saving..." overlay (was a bug
    where the overlay showed during the sensor warm-up on first entry)
  - Rapid successive photo presses: don't queue infinite captures, don't
    panic on semaphore coalescing
  - Photo press while encoder is running in the background
  - Enter/exit camera without taking a photo: no state leak
  - photo_btn → immediately menu out → come back, things should still work

Designed to be resilient to SPI cam flakiness — pass criteria focus on
system stability, not SPI success rates."""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 2, fh)

        # Phase A — first-entry camera page: saving overlay must NOT show
        _lib.mark(fh, 'PHASE A — first enter camera page (no capture yet)')
        _lib.do(s, 'menu_goto camera', 6, fh)
        # Let the sensor warm up while we observe
        _lib.drain(s, 4, fh)
        _lib.do(s, 'status', 2, fh)

        # Phase B — mash photo_btn 3 times quickly
        _lib.mark(fh, 'PHASE B — rapid photo_btn × 3')
        for _ in range(3):
            _lib.do(s, 'photo_btn', 1, fh)
        # Wait for any captures to settle
        _lib.drain(s, 60, fh)
        _lib.do(s, 'status', 2, fh)

        # Phase C — press photo, immediately leave to MAIN, come back
        _lib.mark(fh, 'PHASE C — photo_btn, instant exit, re-enter')
        _lib.do(s, 'photo_btn', 1, fh)
        _lib.do(s, 'menu_goto main', 3, fh)
        _lib.do(s, 'menu_goto camera', 6, fh)
        _lib.do(s, 'status', 2, fh)
        _lib.drain(s, 30, fh)   # let capture finish

        # Phase D — enter camera, don't do anything, exit to gallery
        _lib.mark(fh, 'PHASE D — enter camera → no-op → exit to gallery')
        _lib.do(s, 'menu_goto main', 3, fh)
        _lib.do(s, 'menu_goto camera', 5, fh)
        _lib.do(s, 'menu_goto gifs', 4, fh)
        _lib.do(s, 'ping', 1, fh)

        _lib.do(s, 'menu_goto main', 3, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # Check the "saving overlay during camera entry" regression.
    # The overlay only shows when app_pimslo_is_capturing() is true.
    # At the very start of Phase A, we haven't requested any capture,
    # so the only way the overlay could show is if s_capturing was
    # leaked from a prior run. The tell on-device would be the
    # "saving..." text, but that's a pixel-level thing our log doesn't
    # capture. Best proxy: no "Deferring" events appeared before
    # photo_btn fired.
    phase_a = txt.split('PHASE B')[0] if 'PHASE B' in txt else txt
    false_deferral = phase_a.count('Deferring')
    photo_btn = c['photo_btn']
    captures = c['captures']

    pages = re.findall(r'page=(\w+)', txt)
    last_page = pages[-1] if pages else '?'

    _lib.print_summary('[08] CAMERA EDGE CASES', c, extras={
        'pages': pages,
        'last_page': last_page,
        'deferrals during PHASE A (expect 0)': false_deferral,
    })

    passed = (c['watchdogs'] == 0 and c['panics'] == 0 and
              # ≥4 photo_btn (3 in Phase B, 1 in Phase C)
              photo_btn >= 4 and
              # no encode deferrals reported during phase A (would hint
              # at the overlay showing from stale state)
              false_deferral == 0 and
              last_page == 'MAIN')
    print(f"  VERDICT: {'PASS ✓' if passed else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
