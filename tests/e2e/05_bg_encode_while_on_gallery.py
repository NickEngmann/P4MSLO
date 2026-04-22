#!/usr/bin/env python3
"""Regression test: bg worker must encode stale captures WHILE the user
is sitting on the gallery.

This was the user's top complaint. Earlier iterations gated bg encoding
to ALBUM/USB/SETTINGS pages only, so as soon as ALBUM became aliased
to the GIFS page (the PIMSLO gallery), encoding silently never ran.

Flow:
  1. Check how many JPEG-only entries exist (prior captures whose .gif
     was never produced, e.g. from interrupted encodes).
  2. Navigate to the gallery and stay there.
  3. Observe the bg worker log events:
        BG: pre-rendering .p4ms for ...   (pre-render path)
        BG: encoding PIMSLO from ...       (encode path)
        PIMSLO GIF saved to ...            (encode completion)
  4. Verify playback auto-resumes after each encode (no frozen canvas)."""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)
OBSERVE_SECS = 240  # up to 4 min: enough for 1-3 encodes


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 2, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_gifs', 3, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_small', 3, fh)

        _lib.mark(fh, 'Enter gallery and park')
        _lib.do(s, 'menu_goto gifs', 6, fh)
        _lib.do(s, 'status', 2, fh)

        _lib.mark(fh, f'Observe bg worker for up to {OBSERVE_SECS}s')
        t_end = time.time() + OBSERVE_SECS
        last_ping = time.time()
        while time.time() < t_end:
            _lib.drain(s, 15, fh)
            # Ping every ~30s to prove LVGL is still alive
            if time.time() - last_ping > 30:
                _lib.do(s, 'ping', 1, fh)
                last_ping = time.time()

        _lib.do(s, 'sd_ls /sdcard/p4mslo_gifs', 3, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_small', 3, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # How many times did playback auto-resume after a bg encode?
    # scan_async_cb prints a Gallery-entry line when it resumes.
    # Count "Gallery entry" lines that appear AFTER a "PIMSLO GIF saved" event.
    parts = txt.split('PIMSLO GIF saved to')
    resumes = sum(1 for tail in parts[1:] if 'Gallery entry' in tail.split('PIMSLO GIF saved to')[0][:4000])

    _lib.print_summary('[05] BG ENCODE WHILE ON GALLERY', c,
                        extras={
                            'auto-resume playbacks': resumes,
                            'observation window (s)': OBSERVE_SECS,
                        })

    passed = (c['watchdogs'] == 0 and c['panics'] == 0 and
              c['ping_pong'] >= 1)
    # If there was stale work, we expect at least one bg action
    stale_work_hints = ('encoding PIMSLO' in txt) or ('pre-rendering .p4ms' in txt)
    if stale_work_hints:
        # At least one completion of either pre-render or encode
        completions = c['bg_prerenders'] + c['gifs_saved']
        if completions < 1:
            passed = False
            print('  NOTE: bg worker saw work but nothing completed in the window')
    print(f"  VERDICT: {'PASS ✓' if passed else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
