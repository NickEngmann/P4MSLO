#!/usr/bin/env python3
"""Verifies the knob / up-down buttons actually advance the gallery.

This is a direct regression test for the "knob_rotation early-returned
on ALBUM/USB but didn't dispatch on GIFS" bug — btn_up/btn_down fired
the LVGL handler but the physical knob callbacks in app_control.c had
no branch for UI_PAGE_GIFS.

Requires at least 2 entries. We only simulate the serial-level
btn_up/btn_down which calls ui_extra_btn_{up,down} directly — that
part is always reachable via the serial cmd. The physical knob
rotation goes through knob_left_cb → handle_knob_rotation → action_main
→ ui_extra_btn_up, so knob failures upstream wouldn't show up here.
But the serial level verifies the downstream handler logic is intact."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 2, fh)
        _lib.do(s, 'menu_goto gifs', 6, fh)
        _lib.do(s, 'status', 2, fh)

        _lib.mark(fh, 'Advance through gallery: 5 × btn_down then 3 × btn_up')
        for i in range(5):
            _lib.do(s, 'btn_down', 3, fh)
        for i in range(3):
            _lib.do(s, 'btn_up', 3, fh)

        _lib.do(s, 'status', 2, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    entry_lines = txt.count('Gallery entry')
    entry_indices = re.findall(r'Gallery entry (\d+)/(\d+):', txt)
    distinct_indices = len(set(i for i, _ in entry_indices))
    # What size is the gallery right now? Read from the "M" part of N/M.
    gallery_size = max((int(tot) for _, tot in entry_indices), default=0)

    _lib.print_summary('[04] GALLERY KNOB/BUTTON NAV', c,
                        extras={
                            'gallery size': gallery_size,
                            'total Gallery entry log lines': entry_lines,
                            'distinct indices visited': distinct_indices,
                        })

    # With only 0-1 entries navigation can't cycle — skip rather than fail.
    if gallery_size < 2:
        print('  SKIP: needs ≥ 2 entries to exercise navigation '
              f'(gallery size = {gallery_size})')
        print(f'  log: {LOG}')
        sys.exit(0)

    # With a multi-entry gallery, the 8 press events should visit at
    # least 3 distinct indices (tolerant of wrap + dupe enter log).
    passed = (c['watchdogs'] == 0 and c['panics'] == 0 and
              distinct_indices >= min(3, gallery_size))
    print(f"  VERDICT: {'PASS ✓' if passed else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
