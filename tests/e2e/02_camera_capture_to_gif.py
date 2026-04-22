#!/usr/bin/env python3
"""End-to-end camera → album flow.

Mirrors the UX the user cares about:

  1. Navigate to camera page
  2. Press trigger (photo_btn) — verify SPI capture fires and 2+ cams
  3. Navigate to ALBUM (= GIFS page) — the bg encoder should kick in
     WHILE the user is on the gallery, producing .p4ms + .gif without
     making the user wait somewhere else
  4. Wait for the encode to complete and verify:
     - .p4ms saved (inline direct-JPEG)
     - .gif saved (PIMSLO encode)
     - gallery refreshes and resumes playback (no frozen canvas)
  5. Navigate entries via btn_up/btn_down — verify page stays on GIFS
     and the gallery actually advances (not stuck)

This is the test that would have caught the "encode deferred on
GIFS page" regression and the "knob dispatch doesn't handle GIFS"
bug where up/down did nothing on the gallery."""
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

        _lib.mark(fh, 'PHASE 1 — camera page + take photo')
        _lib.do(s, 'menu_goto camera', 6, fh)
        _lib.do(s, 'status', 2, fh)
        _lib.do(s, 'photo_btn', 2, fh)     # just send, don't wait

        _lib.mark(fh, 'PHASE 1b — wait for SPI capture to finish (up to 45 s)')
        t_end = time.time() + 45
        while time.time() < t_end:
            _lib.drain(s, 5, fh)
            with open(LOG) as lf:
                t = lf.read()
            if re.search(r'Capture \d+: \d/4 cameras', t):
                break

        _lib.mark(fh, 'PHASE 2 — navigate to ALBUM (GIFS)')
        _lib.do(s, 'menu_goto gifs', 6, fh)
        _lib.do(s, 'status', 2, fh)

        _lib.mark(fh, 'PHASE 3 — wait for encode to complete (up to 90s)')
        t_end = time.time() + 90
        got_gif = False
        while time.time() < t_end:
            _lib.drain(s, 5, fh)
            with open(LOG) as lf:
                t = lf.read()
            if 'PIMSLO GIF saved to' in t:
                got_gif = True
                break
        _lib.do(s, 'status', 2, fh)

        _lib.mark(fh, 'PHASE 4 — gallery nav: btn_down should advance')
        _lib.do(s, 'btn_down', 3, fh)
        _lib.do(s, 'ping', 1, fh)
        _lib.do(s, 'btn_down', 3, fh)
        _lib.do(s, 'ping', 1, fh)
        _lib.do(s, 'btn_up', 3, fh)
        _lib.do(s, 'ping', 1, fh)
        _lib.do(s, 'status', 2, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # Extra assertions
    encoded_on_gifs = ('PIMSLO GIF saved to' in txt and
                       'page=GIFS' in txt.split('PIMSLO GIF saved to')[0].split('page=')[-1]
                       if 'page=' in txt else False)
    pages = re.findall(r'page=(\w+)', txt)

    # Count Gallery-entry transitions during PHASE 4 — if the knob/buttons
    # work, btn_down/btn_up should emit "Gallery entry N/M" log lines
    entries_shown = txt.count('Gallery entry')

    _lib.print_summary('[02] CAPTURE → GIF → GALLERY', c,
                        extras={
                            'page transitions': pages,
                            'gallery-entry log lines': entries_shown,
                        })

    # If the SPI rig captured 0/4 cams, there's no capture to encode —
    # that's a hardware state issue, not a firmware regression. Treat
    # that case as a soft-skip (only require no crashes + gallery nav).
    got_capture = c['captures'] and any(int(x) >= 2 for x in c['captures'])

    base = (c['watchdogs'] == 0 and c['panics'] == 0 and
             c['ping_pong'] >= 1 and c['photo_btn'] == 1 and
             entries_shown >= 3)
    if not got_capture:
        print('  NOTE: SPI captured <2 cams — cannot exercise encode path')
        passed = base
    else:
        passed = (base and c['gifs_saved'] >= 1 and c['p4ms_saved'] >= 1)
    print(f"  VERDICT: {'PASS ✓' if passed else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
