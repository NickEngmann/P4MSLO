#!/usr/bin/env python3
"""Diagnostic — NOT part of run_all/run_fast. Fires N back-to-back
`spi_pimslo` captures and reports per-run camera success count +
which specific cameras succeeded/failed. Goal: 20 consecutive 4/4
captures with SPI_MAX_RETRIES=0.

Use: `P4MSLO_TEST_PORT=/dev/ttyACM1 python3 tests/e2e/_spi_20shot.py [N]`
Default N = 20."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

N = int(sys.argv[1]) if len(sys.argv) > 1 else 20
LOG = _lib.log_path(__file__)


def main():
    s = _lib.open_port()
    results = []  # list of (success, cam_successes[4])
    with open(LOG, 'w') as fh:
        _lib.drain(s, 2, fh)
        _lib.reset_state(s, fh, timeout=10)
        _lib.do(s, 'cam_status all', 3, fh)

        for i in range(N):
            _lib.mark(fh, f'run {i+1}/{N}')
            fh_pos = fh.tell()
            _lib.do(s, 'spi_pimslo 150 0.05', 15, fh)
            # Back up, read just this run's window
            fh.flush()
            with open(LOG) as lf:
                lf.seek(fh_pos)
                window = lf.read()
            # Parse per-camera success
            cam_ok = [False, False, False, False]
            for idx in range(4):
                if re.search(rf'Camera {idx+1}: \d+ bytes', window):
                    cam_ok[idx] = True
            succ = sum(cam_ok)
            results.append((succ, cam_ok))
            # Let bg save/encode drain a bit
            time.sleep(1.5)

        _lib.do(s, 'cam_status all', 3, fh)
    s.close()

    # Report
    print()
    print('=' * 60)
    print(f'  SPI 20-SHOT — {N} runs')
    print('=' * 60)
    per_cam = [0, 0, 0, 0]
    full = 0
    for i, (succ, cam_ok) in enumerate(results):
        for j in range(4):
            if cam_ok[j]:
                per_cam[j] += 1
        if succ == 4:
            full += 1
        flags = ''.join('✓' if x else '✗' for x in cam_ok)
        print(f'  run {i+1:2d}: {succ}/4  [{flags}]')
    print('=' * 60)
    print(f'  4/4 runs     : {full} / {N}  ({100*full/N:.0f}%)')
    for j in range(4):
        print(f'  cam {j+1} success : {per_cam[j]} / {N}  ({100*per_cam[j]/N:.0f}%)')
    print('=' * 60)
    print(f'  log: {LOG}')

    sys.exit(0 if full == N else 1)


if __name__ == '__main__':
    main()
