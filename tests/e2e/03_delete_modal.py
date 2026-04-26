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

        # PHASE 0 — fully drain background work + guarantee ≥1 entry.
        # In the suite, prior tests (02, 13, 08) can leave 3+ encodes
        # queued and one mid-encode. If we enter PHASE 1 while a bg
        # encode is in flight, gallery_scan(reasoned by encode-complete)
        # fires mid-modal, resets the modal's NO/YES focus state, and
        # the next btn_trigger is interpreted as "exit gallery to main"
        # instead of "confirm delete". `_lib.reset_state` polls the
        # full pimslo_queue/pimslo_encoding/pimslo_capturing trio
        # against zero before returning. 360 s ≈ 4-5 queued encodes at
        # ~76 s each — comfortable upper bound on suite contention.
        _lib.mark(fh, 'PHASE 0 — drain bg work + ensure gallery non-empty')
        _lib.reset_state(s, fh, timeout=360)
        # If the gallery is already populated from prior tests, skip
        # the photo. Otherwise take one and wait for encode complete.
        _lib.do(s, 'status', 2, fh)
        with open(LOG) as lf:
            tail = lf.read()[-2000:]
        gifs_count = 0
        m = re.search(r'gifs_count=(\d+)', tail)
        if m:
            gifs_count = int(m.group(1))
        if gifs_count == 0:
            _lib.do(s, 'menu_goto camera', 6, fh)
            _lib.do(s, 'photo_btn', 12, fh)
            _lib.do(s, 'menu_goto main', 3, fh)
            # NB: don't `sd_ls` here — that would offset PHASE 1's
            # sd_ls blocks against PHASE 2/3's p4ms blocks and break
            # the zip-aligned `totals` list the verdict logic uses.
            for _ in range(90):  # 90 × 2 s = 180 s — one encode worst
                _lib.do(s, 'status', 2, fh)
                with open(LOG) as lf:
                    tail = lf.read().splitlines()[-20:]
                if any('pimslo_encoding=0' in ln and 'gifs_encoding=0' in ln
                       for ln in tail):
                    break

        _lib.mark(fh, 'PHASE 1 — enter gallery')
        _lib.do(s, 'menu_goto gifs', 6, fh)
        _lib.do(s, 'status', 2, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_small', 3, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_gifs', 3, fh)

        _lib.mark(fh, 'PHASE 2 — open modal; NO default; encoder confirms NO')
        _lib.do(s, 'btn_encoder', 2, fh)
        _lib.do(s, 'btn_encoder', 3, fh)
        # assert still on gallery, no delete
        _lib.do(s, 'status', 2, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_small', 3, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_gifs', 3, fh)

        _lib.mark(fh, 'PHASE 3 — open modal; btn_up → YES; menu confirms → DELETE')
        _lib.do(s, 'btn_encoder', 2, fh)
        _lib.do(s, 'btn_up', 2, fh)
        _lib.do(s, 'btn_trigger', 5, fh)
        time.sleep(3)
        _lib.drain(s, 2, fh)
        _lib.do(s, 'status', 2, fh)
        # Count p4ms AND gifs — the current gallery entry may be .gif-only
        # (no .p4ms yet because bg pre-render hasn't run on it). The
        # delete unlinks whichever files actually exist for that stem,
        # so we verify the total on-disk file count for this capture
        # dropped, not that the .p4ms count specifically dropped.
        _lib.do(s, 'sd_ls /sdcard/p4mslo_small', 3, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_gifs', 3, fh)

        _lib.mark(fh, 'PHASE 4 — menu with NO modal open exits to main')
        # Modal should already be closed by PHASE 3's confirm. Verify menu
        # press now takes us back to main.
        _lib.do(s, 'btn_trigger', 3, fh)
        _lib.do(s, 'status', 2, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # Extract sd_ls blocks for both .p4ms and .gif directories. The
    # deleted entry may be .gif-only (no .p4ms rendered yet) or the
    # reverse, so we add the counts for a stable "total capture
    # files" number that drops by ≥1 across a delete regardless of
    # which file types were attached.
    p4ms_blocks = re.findall(
        r'=== sd_ls /sdcard/p4mslo_small ===(.+?)(?====|\Z)', txt, re.DOTALL)
    gif_blocks = re.findall(
        r'=== sd_ls /sdcard/p4mslo_gifs ===(.+?)(?====|\Z)', txt, re.DOTALL)
    p4ms_counts = [len(re.findall(r'FILE\s+\S+\.p4ms', b)) for b in p4ms_blocks]
    gif_counts  = [len(re.findall(r'FILE\s+\S+\.gif',  b)) for b in gif_blocks]
    totals = [a + b for a, b in zip(p4ms_counts, gif_counts)]
    pages = re.findall(r'page=(\w+)', txt)

    # Expected total-file counts (p4ms + gif):
    #   totals[0] = baseline
    #   totals[1] = after NO cancel → unchanged (= totals[0])
    #   totals[2] = after YES delete → at least one file fewer
    deleted_dropped = (len(totals) >= 3 and totals[0] == totals[1] and
                        totals[2] < totals[0])

    _lib.print_summary('[03] DELETE MODAL', c,
                        extras={
                            'p4ms counts': p4ms_counts,
                            'gif counts':  gif_counts,
                            'total counts': totals,
                            'pages': pages,
                            'NO-then-YES dropped ≥1 file': deleted_dropped,
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
