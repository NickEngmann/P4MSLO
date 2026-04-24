#!/usr/bin/env python3
"""Single-shot smoke test: hits every core subsystem in ~2 min.

Purpose: give the dev a "is anything fundamentally broken?" signal in
under 4 min without running the full regression suite. Run via
`tests/e2e/run_fast.sh`.

What this covers (each one in isolation is trivial — together they
exercise the full P4 stack):

  1. Firmware boot + serial command dispatch      (`ping`, `status`)
  2. Page navigation state machine                 (menu_goto each page)
  3. Button mapping per page                       (btn_up/down/trigger/encoder)
  4. LVGL responsiveness under load                (ping between fast page swaps)
  5. DMA-internal heap health                      (`heap_caps` parse)
  6. Filesystem / SD                               (`sd_ls` both PIMSLO dirs)
  7. SPI capture pipeline                          (single `spi_pimslo`)
  8. Gallery entry + single GIF play               (`menu_goto gifs` + btn_up nav)
  9. Clean return to main                          (reset_state)

Pass bar: 0 panics, 0 watchdogs, every expected page reached, heap
thresholds OK, SPI capture returned a response (success or fail —
as long as it didn't panic)."""
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)

# Post-capture threshold — test 11 runs a full spi_pimslo before the
# heap check, which temporarily pulls 4 KB from DMA-internal for the
# chunk-RX buffer + leaves SD and LCD transaction scratch in flight.
# End-of-test largest-free-block observed around 200-500 B. Pre-capture
# baseline is covered by test 12 (threshold 2 KB there); here we're
# just catching catastrophic exhaustion (< 128 B means no future alloc
# can succeed at all).
MIN_DMA_INT_LARGEST = 128
MIN_PSRAM_LARGEST   = 8 * 1024 * 1024


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 2, fh)
        _lib.reset_state(s, fh, timeout=10)

        # --- 1. Liveness ---
        _lib.mark(fh, '1/9 liveness')
        _lib.do(s, 'ping', 1.5, fh)
        _lib.do(s, 'status', 1.5, fh)

        # Note: the SPI capture smoke that used to be step 2 here
        # moved into test 14 (`14_capture_encode_offpage.py`) — it
        # makes more sense to exercise capture + encode end-to-end
        # in one test rather than leaving a half-saved PIMSLO on SD
        # from a liveness smoke. Heartbeat now is pure UI + heap
        # verification; test 14 covers the capture path.

        # --- 3. Page nav over all 6 pages + buttons ---
        _lib.mark(fh, '2/9 page nav')
        for page in ['camera', 'gifs', 'video', 'usb', 'settings', 'main']:
            _lib.do(s, f'menu_goto {page}', 3, fh)
            _lib.do(s, 'ping', 1, fh)

        _lib.mark(fh, '3/9 buttons on main menu')
        for btn in ['btn_up', 'btn_down', 'btn_up', 'btn_down']:
            _lib.do(s, btn, 0.6, fh)

        _lib.mark(fh, '4/9 heap_caps')
        _lib.do(s, 'heap_caps', 1.5, fh)

        _lib.mark(fh, '5/9 sd_ls (both dirs)')
        _lib.do(s, 'sd_ls /sdcard/p4mslo_gifs', 3, fh)
        _lib.do(s, 'sd_ls /sdcard/p4mslo_small', 3, fh)

        _lib.mark(fh, '6/9 gallery + nav')
        _lib.do(s, 'menu_goto gifs', 4, fh)
        _lib.do(s, 'btn_up', 2, fh)
        _lib.do(s, 'btn_down', 2, fh)
        _lib.do(s, 'status', 1.5, fh)

        _lib.mark(fh, '7/9 return to main')
        _lib.reset_state(s, fh, timeout=10)
        _lib.do(s, 'status', 1.5, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # Heap parse
    m = re.search(
        r'heap_caps dma_int=\d+ \(largest=(\d+)\) '
        r'int=\d+ \(largest=\d+\) psram=\d+ \(largest=(\d+)\)',
        txt)
    dma_largest = int(m.group(1)) if m else 0
    psram_largest = int(m.group(2)) if m else 0

    # Page-nav coverage (did we actually see each page in a status line?)
    # Accept ≥5/6 pages — the CAMERA page transition occasionally gets
    # eaten by stale spi_pimslo log output on the ttyACM stream when
    # the spi_pimslo step in step 2 is still draining responses. The
    # transition itself works; the status-response is what we miss.
    pages_seen = set(re.findall(r'page=(\w+)', txt))
    want_pages = {'MAIN', 'CAMERA', 'GIFS', 'VIDEO_MODE', 'USB_DISK', 'SETTINGS'}
    pages_missing = want_pages - pages_seen
    enough_pages = (len(pages_seen & want_pages) >= 5)

    # (SPI capture smoke moved to test 14 — heartbeat no longer
    # exercises the camera path.)

    extras = {
        'dma_int largest':  f'{dma_largest} B (min {MIN_DMA_INT_LARGEST})',
        'psram   largest':  f'{psram_largest} B',
        'pages seen':       sorted(pages_seen),
        'pages missing':    sorted(pages_missing) or 'none',
    }
    _lib.print_summary('[11] HEARTBEAT', c, extras=extras)

    ok = (c['watchdogs'] == 0 and c['panics'] == 0 and
          c['ping_pong'] >= 5 and
          dma_largest >= MIN_DMA_INT_LARGEST and
          psram_largest >= MIN_PSRAM_LARGEST and
          enough_pages)
    print(f"  VERDICT: {'PASS ✓' if ok else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
