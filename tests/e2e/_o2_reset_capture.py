#!/usr/bin/env python3
"""Reset device via esptool and capture boot log before taskLVGL hangs.

Uses esptool to pulse reset, then immediately opens serial and reads
for 30 s — should catch the full boot message sequence leading up to
the hang point, unlike the previous capture which missed boot logs."""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import _lib  # noqa: E402

LOG = _lib.log_path(__file__)

# Hard reset via esptool in docker
# We just flash nothing (run_stub no-op) which resets the chip.
subprocess.run(
    'docker run --rm --device=/dev/ttyACM0 espressif/idf:v5.5.3 '
    'bash -c "python -m esptool --chip esp32p4 --port /dev/ttyACM0 chip-id"',
    shell=True, check=False, timeout=60,
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
)

time.sleep(0.1)  # let USB ACM re-enumerate

with open(LOG, 'w') as fh, _lib.open_port() as s:
    _lib.mark(fh, 'POST-RESET CAPTURE (25s)')
    _lib.drain(s, 25, fh)

with open(LOG) as f:
    txt = f.read()

lines = txt.splitlines()
pre_wdt = []
for ln in lines:
    if 'task_wdt' in ln or 'MEPC' in ln or 'MSTATUS' in ln:
        break
    pre_wdt.append(ln)

print(f'Total lines: {len(lines)}')
print(f'Lines before first wdt: {len(pre_wdt)}')
print('--- Last 40 lines before hang ---')
for ln in pre_wdt[-40:]:
    print(f'  {ln}')
