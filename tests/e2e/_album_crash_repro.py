#!/usr/bin/env python3
"""Repro: open album repeatedly and capture any crash output.

Diagnostic only — reports backtrace if one appears. Not part of run_all.sh.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _lib  # noqa: E402

LOG = _lib.log_path(__file__)

with open(LOG, 'w') as fh, _lib.open_port() as s:
    _lib.mark(fh, 'BOOT WAIT 6s')
    _lib.drain(s, 6, fh)
    _lib.do(s, 'ping', 1, fh)
    _lib.do(s, 'status', 1, fh)

    # Try opening album 8 times, back to main between each.
    # The UI "Album" item maps to UI_PAGE_GIFS (PIMSLO gallery).
    for i in range(8):
        _lib.mark(fh, f'OPEN ALBUM #{i+1}')
        _lib.do(s, 'menu_goto gifs', 3.0, fh)
        _lib.do(s, 'status', 1, fh)
        _lib.mark(fh, f'BACK TO MAIN #{i+1}')
        _lib.do(s, 'menu_goto main', 2, fh)
        _lib.do(s, 'status', 0.5, fh)

    _lib.do(s, 'ping', 1, fh)

with open(LOG) as f:
    txt = f.read()

c = _lib.summarize(txt)
panics = [line for line in txt.splitlines()
          if 'Guru Meditation' in line
          or 'abort()' in line
          or 'Backtrace:' in line
          or 'assert failed' in line
          or 'rst:' in line
          or 'task_wdt' in line]
_lib.print_summary('Album-open crash repro', c,
                   {'crash_lines': len(panics)})
for p in panics[:20]:
    print('    ', p)
_lib.verdict(c)
