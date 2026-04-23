#!/usr/bin/env python3
"""Aggressive album-open crash repro.

The casual menu_goto / status / menu_goto main loop does NOT crash —
that was already covered. This tries harder:

  - Open album immediately after a fresh photo (race with preview save)
  - Hammer btn_up/btn_down on the gallery (LZW decoder stress)
  - Toggle into and out of the delete modal rapidly
  - Re-enter album while bg encoder is still running
  - 40 cycles total
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _lib  # noqa: E402

LOG = _lib.log_path(__file__)

with open(LOG, 'w') as fh, _lib.open_port() as s:
    _lib.drain(s, 3, fh)
    _lib.do(s, 'ping', 1, fh)
    _lib.do(s, 'status', 1, fh)

    # Phase A: hammer open/close 20x
    _lib.mark(fh, 'A - hammer open/close 20x')
    for i in range(20):
        _lib.do(s, 'menu_goto gifs', 0.8, fh)
        _lib.do(s, 'menu_goto main', 0.5, fh)

    # Phase B: open album + rapid btn_down/up
    _lib.mark(fh, 'B - rapid nav within album')
    _lib.do(s, 'menu_goto gifs', 1.5, fh)
    for i in range(15):
        _lib.do(s, 'btn_down', 0.4, fh)
    for i in range(15):
        _lib.do(s, 'btn_up', 0.4, fh)

    # Phase C: toggle delete modal rapidly
    _lib.mark(fh, 'C - delete modal toggle stress')
    for i in range(6):
        _lib.do(s, 'btn_trigger', 0.6, fh)   # open modal
        _lib.do(s, 'btn_up', 0.3, fh)        # switch YES/NO
        _lib.do(s, 'btn_down', 0.3, fh)
        _lib.do(s, 'btn_menu', 0.6, fh)      # confirm NO (close)

    # Phase D: take a photo, then immediately open album mid-capture
    _lib.mark(fh, 'D - capture-race: photo then album')
    _lib.do(s, 'menu_goto camera', 2, fh)
    _lib.do(s, 'photo_btn', 0.2, fh)         # don't wait — race
    _lib.do(s, 'menu_goto gifs', 4, fh)      # open immediately

    # Phase E: bounce gallery while bg encoder is running
    _lib.mark(fh, 'E - bounce while encoding')
    for i in range(6):
        _lib.do(s, 'menu_goto main', 0.5, fh)
        _lib.do(s, 'menu_goto gifs', 1.2, fh)

    _lib.do(s, 'ping', 1, fh)
    _lib.do(s, 'status', 2, fh)

with open(LOG) as f:
    txt = f.read()

c = _lib.summarize(txt)
gurus = txt.count('Guru Meditation')
wdts = txt.count('task_wdt')
reboots = txt.count('rst:') + txt.count('ets_main.c:') + txt.count('Rebooting')
_lib.print_summary('Album-open crash stress', c,
                   {'Guru': gurus, 'reboots': reboots})

ok = (c['watchdogs'] == 0 and c['panics'] == 0 and reboots == 0)
print(f'  VERDICT: {"PASS ✓" if ok else "FAIL ✗"}')
print(f'  log: {LOG}')
sys.exit(0 if ok else 1)
