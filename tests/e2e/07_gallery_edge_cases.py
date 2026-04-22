#!/usr/bin/env python3
"""Gallery edge cases — quick-fire button sequences the user exercises
in real life.

Guards against:
  - Button mash: many rapid btn_down with no drain between → does the
    page freeze or the handler drop events?
  - Button race: btn_down immediately followed by btn_trigger → does
    the modal open on stale state?
  - Modal rapid toggle: btn_encoder / btn_up / btn_encoder / btn_up
    rapidly → is the selection state coherent?
  - Menu escape from modal on empty gallery → never stuck
  - Repeated gallery-enter/exit from MAIN → no state leak

Target: zero watchdogs, zero panics, ping stays responsive throughout."""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)


def fast_do(s, cmd, fh, wait=0.05):
    """Send a command with minimal drain — mash-style."""
    s.write((cmd + '\n').encode()); s.flush()
    fh.write(f'\n=== {cmd} ===\n'); fh.flush()
    time.sleep(wait)
    # Opportunistic drain without blocking
    try:
        d = s.read(4096)
        if d: fh.write(d.decode('utf-8', 'replace')); fh.flush()
    except Exception:
        pass


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 2, fh)

        # Phase A: rapid-fire btn_down in gallery
        _lib.mark(fh, 'PHASE A — rapid btn_down × 20 in gallery')
        _lib.do(s, 'menu_goto gifs', 5, fh)
        for _ in range(20):
            fast_do(s, 'btn_down', fh, wait=0.08)
        _lib.drain(s, 3, fh)
        _lib.do(s, 'ping', 1, fh)
        _lib.do(s, 'status', 2, fh)

        # Phase B: alternating btn_down + btn_trigger rapidly
        _lib.mark(fh, 'PHASE B — interleave btn_down and btn_trigger × 10')
        for _ in range(10):
            fast_do(s, 'btn_down', fh, wait=0.05)
            fast_do(s, 'btn_trigger', fh, wait=0.1)
        _lib.drain(s, 5, fh)
        _lib.do(s, 'ping', 1, fh)
        _lib.do(s, 'status', 2, fh)

        # Phase C: rapid modal dance: encoder (open) → up → encoder (close
        # on whatever state that leaves us) — many times quickly
        _lib.mark(fh, 'PHASE C — modal rapid toggle × 15')
        for _ in range(15):
            fast_do(s, 'btn_encoder', fh, wait=0.1)
            fast_do(s, 'btn_up', fh, wait=0.05)
            fast_do(s, 'btn_encoder', fh, wait=0.2)
        _lib.drain(s, 5, fh)
        _lib.do(s, 'ping', 1, fh)
        _lib.do(s, 'status', 2, fh)

        # Phase D: gallery enter/exit cycle stressing leaving_main path
        _lib.mark(fh, 'PHASE D — gallery ↔ main × 8')
        for _ in range(8):
            fast_do(s, 'menu_goto main', fh, wait=0.5)
            fast_do(s, 'menu_goto gifs', fh, wait=0.5)
        _lib.drain(s, 3, fh)
        _lib.do(s, 'ping', 1, fh)

        # Phase E: open modal then navigate away without confirming
        _lib.mark(fh, 'PHASE E — open modal, jump to MAIN before confirming')
        _lib.do(s, 'btn_encoder', 2, fh)   # open modal
        _lib.do(s, 'menu_goto main', 3, fh) # jump away; modal should hide
        _lib.do(s, 'menu_goto gifs', 4, fh) # come back
        _lib.do(s, 'ping', 1, fh)
        _lib.do(s, 'status', 2, fh)

        _lib.do(s, 'menu_goto main', 3, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    ping_count = c['ping_pong']
    pages = re.findall(r'page=(\w+)', txt)
    # Did every "menu_goto X" end up at the right page?
    transitions = re.findall(r"Command: 'menu_goto (\w+)'", txt)
    last_page = pages[-1] if pages else '?'

    _lib.print_summary('[07] GALLERY EDGE CASES', c, extras={
        'ping responses across 5 phases': ping_count,
        'menu transitions invoked': len(transitions),
        'final page': last_page,
    })

    passed = (c['watchdogs'] == 0 and c['panics'] == 0 and
              ping_count >= 5 and last_page == 'MAIN')
    print(f"  VERDICT: {'PASS ✓' if passed else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
