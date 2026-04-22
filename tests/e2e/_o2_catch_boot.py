#!/usr/bin/env python3
"""Open serial FIRST, then trigger a hard reset via DTR/RTS pulse so
we catch boot output from t=0. The earlier script opened serial too
late and missed everything."""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _lib  # noqa: E402

LOG = _lib.log_path(__file__)

import serial
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.1)
# ESP32 RTS reset pulse (idf convention for RTS-resetting boards)
s.rts = True
s.dtr = False
time.sleep(0.1)
s.rts = False
time.sleep(0.5)

with open(LOG, 'w') as fh:
    _lib.mark(fh, 'POST-RESET 15s')
    _lib.drain(s, 15, fh)
    _lib.do(s, 'ping', 2, fh)
    _lib.do(s, 'status', 2, fh)

s.close()

with open(LOG) as f:
    txt = f.read()
lines = txt.splitlines()
print(f'lines total: {len(lines)}')
print(f'pong: {txt.count("pong")}')
print(f'wdt: {txt.count("task_wdt")}')
print(f'panic: {txt.count("Guru Meditation")}')
print(f'Backtrace: {txt.count("Backtrace")}')
print('--- First 40 lines ---')
for ln in lines[:40]:
    print(f'  {ln}')
print('--- Last 30 lines ---')
for ln in lines[-30:]:
    print(f'  {ln}')
