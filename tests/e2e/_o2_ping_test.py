#!/usr/bin/env python3
"""Quick ping test — is the P4 responsive after the -O2/lvgl-Og flash?"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _lib  # noqa: E402

LOG = _lib.log_path(__file__)

with open(LOG, 'w') as fh, _lib.open_port() as s:
    time.sleep(0.3)
    s.reset_input_buffer()
    _lib.mark(fh, 'WAIT 5s then ping 3x')
    _lib.drain(s, 5, fh)
    for i in range(3):
        _lib.do(s, 'ping', 1.5, fh)
    _lib.do(s, 'status', 2, fh)

with open(LOG) as f:
    txt = f.read()
print(f'Log chars: {len(txt)}')
print(f'pong count: {txt.count("pong")}')
print(f'fw= line: {"fw=" in txt}')
print('--- Log ---')
print(txt[-1500:])
