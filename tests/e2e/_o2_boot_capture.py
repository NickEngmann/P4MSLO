#!/usr/bin/env python3
"""Capture full boot log from -O2 firmware so we can see the hang
backtrace (if any). The board was freshly flashed and reset via RTS
before this runs — we read for ~45 s and dump everything.

Emits to _o2_boot_capture.log. Then prints a summary of interesting
lines (task_wdt, panic, assert, backtrace)."""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _lib  # noqa: E402

LOG = _lib.log_path(__file__)

with open(LOG, 'w') as fh, _lib.open_port() as s:
    _lib.mark(fh, 'BOOT CAPTURE (45s)')
    _lib.drain(s, 45, fh)
    _lib.do(s, 'ping', 1, fh)
    _lib.do(s, 'status', 1, fh)

with open(LOG) as f:
    txt = f.read()

interesting = []
for line in txt.splitlines():
    if any(m in line for m in [
        'task_wdt', 'TWDT', 'Guru Meditation', 'panic',
        'Backtrace', 'Stack', 'abort()',
        'assert failed', 'rst:', 'Rebooting', 'IDLE',
        'CPU 0:', 'CPU 1:',
        'Register dump', 'Exception', 'MCAUSE',
    ]):
        interesting.append(line)

print(f'Log length: {len(txt)} chars, {len(txt.splitlines())} lines')
print(f'ping_pong count: {txt.count("pong")}')
print(f'status returned: {"fw=" in txt}')
print('')
print(f'Interesting lines ({len(interesting)}):')
for ln in interesting[:80]:
    print(f'  {ln}')
