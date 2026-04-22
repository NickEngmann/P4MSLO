#!/usr/bin/env python3
"""Boot + basic liveness.

Exits PASS if:
  - Device boots without watchdog / panic
  - `status` responds
  - `ping` responds with pong
  - cam_status returns data for all 4 slots (whether the SPI cams are
    plugged in or not — if the firmware is healthy it still replies)

This is the smoke test — run it after every flash before any deeper test."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 3, fh)    # let any straggling boot output in
        _lib.do(s, 'ping', 2, fh)
        _lib.do(s, 'status', 2, fh)
        _lib.do(s, 'cam_status', 5, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)
    ok_cam_status = 'cam 1:' in txt and 'cam 4:' in txt
    _lib.print_summary('[01] BOOT + LIVENESS', c,
                        extras={'cam_status replied': ok_cam_status})
    passed = (c['watchdogs'] == 0 and c['panics'] == 0 and
              c['ping_pong'] >= 1 and ok_cam_status)
    print(f"  VERDICT: {'PASS ✓' if passed else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
