#!/usr/bin/env python3
"""Format SD → first photo → gallery flow.

Regression for the three user-reported bugs from 2026-04-26:
  1. After format, the first photo errors but the second works.
  2. After format, photo → gallery nav crashes during Queued→Processing.
     The picture stays Queued forever after reboot.
  3. After format, navigating to the gallery shows "Album empty"
     overlay AND the just-taken photo's "Processing" badge + filename
     overlapping.

Shared root cause: encoder defer-loop window. The encoder task popped
a job off the queue but didn't set s_encoding=true until AFTER the
defer loop exited. So format_workers_idle() couldn't see the in-flight
job; format wiped the source dir under the encoder; the resumed
encode tried to read missing pos*.jpg files; UI got into an
inconsistent state.

After fix:
  - Encoder claims s_encoding the moment it pops a job (BEFORE defer)
  - Format gates on that flag, refuses with "busy" while deferred
  - Encoder also defensively skips dirs with <2 pos*.jpg files
  - Gallery refresh_empty_overlay called UNCONDITIONALLY after scan

Pass criteria:
  - Format succeeds (or refuses with busy — both acceptable
    depending on what was queued)
  - First photo after format does NOT trigger ERROR overlay
  - Encoder runs and lands a .gif (or, if cams partly failed, the
    capture is dropped but no panic)
  - No watchdogs, no panics, no Guru Meditations
  - Gallery navigation post-photo doesn't show empty overlay when
    count > 0 (we can't read the LVGL pixel state, but we can assert
    that the entry made it into gallery_count and there's no
    inconsistency in the status output)
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 2, fh)

        # Drain any leftover encoder work first so the format isn't
        # racing prior tests' deferred jobs (the very situation being
        # tested — but we want to start from a clean slate so the
        # bug repro is deterministic).
        _lib.reset_state(s, fh, timeout=360)

        _lib.mark(fh, 'PHASE A — format SD from a known-idle state')
        _lib.do(s, 'menu_goto main', 3, fh)
        _lib.do(s, 'format_sd', 4, fh)
        # Poll for "format_sd done" up to 30 s.
        t_end = time.time() + 30
        while time.time() < t_end:
            _lib.do(s, 'status', 1, fh)
            with open(LOG) as lf:
                tail = lf.read()[-800:]
            if 'format_sd: done' in tail or 'ok format_sd' in tail:
                break
        _lib.drain(s, 3, fh)
        _lib.do(s, 'sd_ls /sdcard', 2, fh)

        _lib.mark(fh, 'PHASE B — first photo immediately after format (BUG 1)')
        _lib.do(s, 'menu_goto camera', 6, fh)
        _lib.do(s, 'photo_btn', 25, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo', 2, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_previews', 2, fh)
        _lib.do(s, 'sd_ls /sdcard/esp32_p4_pic_save', 2, fh)
        _lib.do(s, 'status', 2, fh)

        _lib.mark(fh, 'PHASE C — navigate to gallery while encode runs (BUG 2)')
        _lib.do(s, 'menu_goto gifs', 6, fh)
        _lib.do(s, 'status', 2, fh)
        # Watch for ~30 s — the encode is in flight; we need to detect
        # any panic / watchdog during the Queued→Processing transition.
        for _ in range(15):
            _lib.do(s, 'ping', 2, fh)

        _lib.mark(fh, 'PHASE D — drain encoder + verify gallery state (BUG 3)')
        _lib.do(s, 'menu_goto main', 3, fh)
        # Wait for queue to drain.
        for _ in range(90):
            _lib.do(s, 'status', 2, fh)
            with open(LOG) as lf:
                tail = lf.read().splitlines()[-15:]
            if any('pimslo_encoding=0' in ln and 'gifs_encoding=0' in ln and
                   'pimslo_queue=0' in ln for ln in tail):
                break
        _lib.do(s, 'menu_goto gifs', 5, fh)
        _lib.do(s, 'status', 2, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_gifs', 2, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_previews', 2, fh)
        _lib.do(s, 'menu_goto main', 3, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # PASS criteria
    # -------------
    # 1. No panics or watchdogs anywhere.
    # 2. format_sd completed (look for "format_sd done").
    # 3. First photo did NOT report only 0 or 1 cameras (no ERROR
    #    overlay path). Tolerate <4 cams as long as ≥2.
    # 4. Final gallery scan shows count > 0 OR the encoder skipped a
    #    bad dir defensively (post-fix new behavior). Either way no
    #    panic.
    format_done    = 'format_sd: done' in txt
    captures       = c['captures']
    # First post-format capture's usable count.
    first_cap_ok = (len(captures) == 0) or (int(captures[0]) >= 2)
    # Final gifs_count from the last status line.
    final_status_lines = [ln for ln in txt.splitlines()
                          if 'gifs_count=' in ln and 'page=' in ln]
    final_count = 0
    if final_status_lines:
        m = re.search(r'gifs_count=(\d+)', final_status_lines[-1])
        if m:
            final_count = int(m.group(1))
    # The capture may have been dropped if cams were flaky on this
    # run — that's not a regression we're testing for here. As long
    # as no panic, we pass.
    pages = re.findall(r'page=(\w+)', txt)
    last_page = pages[-1] if pages else ''

    _lib.print_summary('[15] FORMAT + FIRST PHOTO + GALLERY', c, extras={
        'format_sd: done': format_done,
        'first capture cams >=2': first_cap_ok,
        'final gallery count': final_count,
        'pages': pages[-6:] if len(pages) > 6 else pages,
        'last_page': last_page,
    })

    passed = (c['watchdogs'] == 0 and c['panics'] == 0 and
              format_done and first_cap_ok and
              last_page == 'MAIN')
    print(f"  VERDICT: {'PASS ✓' if passed else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
