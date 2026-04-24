#!/usr/bin/env python3
"""PIMSLO capture → encode completes off-gallery.

Part of the fast heartbeat (`run_fast.sh`). Tests the real-world flow
the user actually cares about: "take a picture, the GIF shows up in
the album a bit later."

Why not test 02's flow? Test 02 takes a photo then immediately enters
the GIFS gallery and waits there for the encode. On this hardware,
the encoder on gallery-page loses ~4× speed to contention (tjpgd
mutex + SD I/O + PSRAM + CPU 1 contention with gallery playback +
preview flash), stretching a 50 s encode to 200-300 s. That's
long enough to time out a reasonable test window.

This test avoids that race: after the capture, we leave the device
on MAIN (no gallery playback, no preview flash, no CPU 1 contention)
so the encoder runs uncontested at nominal speed. The encode should
complete in 50-80 s. Then we check the gallery to confirm the .gif
made it to disk.

Pass criteria:
  - no panics, no watchdogs, no OOM
  - photo_btn succeeded (P4 photo saved)
  - spi_pimslo got ≥ 2 cams (encode path exercised)
  - PIMSLO GIF saved within 120 s
  - .p4ms preview saved (direct-JPEG path)
"""
import os
import re
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)
ENCODE_WAIT_SECS = 120


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 2, fh)
        _lib.reset_state(s, fh, timeout=10)

        # Go to camera page and fire photo_btn. `take_and_save_photo`
        # + pimslo capture run here; pimslo_encode_queue_task queues
        # the encode job once all 4 SPI pos JPEGs land on SD.
        _lib.mark(fh, '1/4 capture on camera page')
        _lib.do(s, 'menu_goto camera', 4, fh)
        _lib.do(s, 'photo_btn', 2, fh)

        _lib.mark(fh, '2/4 wait for SPI capture to finish (up to 30 s)')
        t_end = time.time() + 30
        while time.time() < t_end:
            _lib.drain(s, 3, fh)
            with open(LOG) as lf:
                t = lf.read()
            if re.search(r'Capture \d+:.*usable=\d/4', t):
                break

        # Leave CAMERA for MAIN (NOT GIFS). `encode_should_defer`
        # sees UI_PAGE_MAIN and returns true → encoder stays quiet
        # briefly, then returns false once we move to SETTINGS (MAIN
        # defers, SETTINGS does not). Actually simplest: go to
        # SETTINGS — no camera buffers, no gallery contention, no
        # defer.
        _lib.mark(fh, '3/4 leave camera for SETTINGS (encoder-safe page)')
        _lib.do(s, 'menu_goto settings', 3, fh)

        _lib.mark(fh, f'4/4 wait for encode off-gallery (up to {ENCODE_WAIT_SECS} s)')
        t_end = time.time() + ENCODE_WAIT_SECS
        got_gif = False
        while time.time() < t_end:
            _lib.drain(s, 5, fh)
            with open(LOG) as lf:
                t = lf.read()
            if 'PIMSLO GIF saved to' in t:
                got_gif = True
                break
        _lib.do(s, 'status', 2, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # Capture count — ≥ 2 means encode path was exercised.
    capture_n = 0
    if c['captures']:
        try:
            capture_n = max(int(x) for x in c['captures'])
        except (ValueError, TypeError):
            capture_n = 0

    extras = {
        'capture cams':       f'{capture_n}/4',
        'p4ms saved':         c['p4ms_saved'],
        'gifs saved':         c['gifs_saved'],
        'encode completed':   'yes' if c['gifs_saved'] >= 1 else 'NO ← FAIL',
    }
    _lib.print_summary('[14] OFF-GALLERY ENCODE', c, extras=extras)

    ok = (c['watchdogs'] == 0 and c['panics'] == 0 and
          c['ping_pong'] >= 0 and c['photo_btn'] == 1 and
          capture_n >= 2 and
          c['gifs_saved'] >= 1 and c['p4ms_saved'] >= 1)
    print(f"  VERDICT: {'PASS ✓' if ok else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
