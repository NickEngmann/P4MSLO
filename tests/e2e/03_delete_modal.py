#!/usr/bin/env python3
"""Delete-modal confirmation flow on the gallery.

Button semantics the user explicitly called out:
  - Encoder/trigger on gallery (modal closed) → opens delete modal
  - Menu button on gallery (modal closed) → exits to main (guaranteed
    escape hatch so the user is never trapped inside the modal)
  - Both encoder AND menu confirm when modal is open
  - Up/down toggle YES/NO when modal is open

This test goes through each case:
  - Open modal via encoder; confirm NO (default) via encoder → no delete
  - Open modal; btn_up toggles to YES; menu confirms → delete
  - Open modal; btn_trigger (menu) exits to main

Requires at least one entry in the gallery."""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)


def count_p4ms_files(txt):
    """Extract the last sd_ls of /sdcard/p4mslo_small and count entries."""
    blocks = re.findall(r'=== sd_ls /sdcard/p4mslo_small ===(.+?)(?====|\Z)',
                        txt, re.DOTALL)
    if not blocks:
        return 0
    return len(re.findall(r'FILE\s+\S+\.p4ms', blocks[-1]))


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 2, fh)

        _lib.mark(fh, 'PHASE 1 — enter gallery')
        _lib.do(s, 'menu_goto gifs', 6, fh)
        _lib.do(s, 'status', 2, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_small', 3, fh)

        _lib.mark(fh, 'PHASE 2 — open modal; NO default; encoder confirms NO')
        _lib.do(s, 'btn_encoder', 2, fh)
        _lib.do(s, 'btn_encoder', 3, fh)
        # assert still on gallery, no delete
        _lib.do(s, 'status', 2, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_small', 3, fh)

        _lib.mark(fh, 'PHASE 3 — open modal; btn_up → YES; menu confirms → DELETE')
        _lib.do(s, 'btn_encoder', 2, fh)
        _lib.do(s, 'btn_up', 2, fh)
        _lib.do(s, 'btn_trigger', 5, fh)
        time.sleep(3)
        _lib.drain(s, 2, fh)
        _lib.do(s, 'status', 2, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_small', 3, fh)

        _lib.mark(fh, 'PHASE 4 — menu with NO modal open exits to main')
        # Modal should already be closed by PHASE 3's confirm. Verify menu
        # press now takes us back to main.
        _lib.do(s, 'btn_trigger', 3, fh)
        _lib.do(s, 'status', 2, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # Extract sd_ls blocks to count file changes
    all_blocks = re.findall(
        r'=== sd_ls /sdcard/p4mslo_small ===(.+?)(?====|\Z)', txt, re.DOTALL)
    counts = [len(re.findall(r'FILE\s+\S+\.p4ms', b)) for b in all_blocks]
    pages = re.findall(r'page=(\w+)', txt)

    # Expected:
    #   counts[0] = baseline
    #   counts[1] = after NO cancel → unchanged (= counts[0])
    #   counts[2] = after YES delete → -1
    deleted_dropped = (len(counts) >= 3 and counts[0] == counts[1] and
                        counts[2] == counts[0] - 1)

    _lib.print_summary('[03] DELETE MODAL', c,
                        extras={
                            'p4ms counts': counts,
                            'pages': pages,
                            'NO-then-YES dropped exactly 1': deleted_dropped,
                        })

    # PHASE 2 should leave us on GIFS; PHASE 4 should leave us on MAIN
    last_page = pages[-1] if pages else ''
    passed = (c['watchdogs'] == 0 and c['panics'] == 0 and
              c['deletes'] >= 1 and deleted_dropped and
              last_page == 'MAIN')
    print(f"  VERDICT: {'PASS ✓' if passed else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
