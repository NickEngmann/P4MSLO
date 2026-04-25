#!/usr/bin/env python3
"""Back-to-back SPI captures — fragmentation + priv-buf alloc regression.

What broke this session (2026-04-24):

`setup_dma_priv_buffer(1206): Failed to allocate priv RX buffer` →
Guru Meditation (Load access fault) fired around camera 4 of the FIRST
`spi_pimslo` run after boot. Root cause: ESP-IDF's SPI master falls
back to a per-transaction priv-buffer alloc whenever the source/dest
isn't 64-byte cache-aligned. Our `heap_caps_malloc(MALLOC_CAP_DMA |
MALLOC_CAP_INTERNAL)` returns a smaller alignment, so every small poll
transaction paid the priv-alloc cost until the internal pool
fragmented and the next alloc returned NULL → panic.

Two signals regress this independently:

1. `setup_dma_priv_buffer` must NEVER fire — if it does, even once,
   we're paying the per-xfer alloc cost and the panic is one bad day
   away. The fix in `spi_camera.c` (64-byte aligned scratch + chunk
   RX buffers) makes this literal-zero under normal operation.

2. Running `spi_pimslo` five times in a row with no panics, no
   watchdogs, and no monotonic drop in the DMA-internal largest-free-
   block. Pre-fix, this test would reliably panic by run 2.

This test runs even with ZERO S3 cameras attached — the P4-side pool
management is what we're checking. Captures will return 0/4 but that
is expected and does NOT fail the test. What fails it is panics or
the priv-buffer log.

Runtime: ~90 s (5 × ~12 s capture windows + polls)."""
import os
import re
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)
N_CAPTURES = 5

# If DMA-internal largest-free-block drops by more than this across
# 5 captures, something is leaking internal RAM each shot.
MAX_DMA_INT_DRIFT = 2048


def parse_dma_largest(txt, after_ts):
    """Find the first `heap_caps dma_int=... (largest=Y) ...` log line
    whose device-timestamp is >= after_ts. Returns int or None."""
    for line in txt.splitlines():
        m = re.search(
            r'heap_caps dma_int=\d+ \(largest=(\d+)\)', line)
        if m:
            return int(m.group(1))
    return None


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 3, fh)
        _lib.do(s, 'ping', 2, fh)

        _lib.mark(fh, 'baseline heap_caps BEFORE captures')
        _lib.do(s, 'heap_caps', 2, fh)

        for i in range(N_CAPTURES):
            _lib.mark(fh, f'spi_pimslo run {i+1}/{N_CAPTURES}')
            # 15 s window — `spi_pimslo` returns the capture_ms summary
            # within 4-8 s normally; with all 4 cams timing out it takes
            # up to 10 s before giving up. 15 s covers both.
            _lib.do(s, 'spi_pimslo 150 0.05', 15, fh)
            # Let the background save/encode queue drain a bit between
            # runs — the panic was triggered during capture, not encode,
            # so short sleeps are fine here.
            time.sleep(1)

        # Drain to encoder idle BEFORE measuring drift. The encoder's
        # working set (7 MB scaled_buf + 23 KB err buffers + JPEG buf)
        # consumes ~20 KB of internal RAM while running, which would
        # otherwise show up as a false-positive "leak" of dma_int. Wait
        # up to 300 s for pimslo_encoding=0 + gifs_encoding=0; the last
        # encode in a 5-shot burst can take 80-120 s on top of any
        # earlier ones still in the queue.
        _lib.mark(fh, 'wait for encoder idle before sampling drift')
        for _ in range(150):
            _lib.do(s, 'status', 2, fh)
            time.sleep(0)
            with open(LOG) as lf:
                tail = lf.read().splitlines()[-20:]
            if any('pimslo_encoding=0' in ln and 'gifs_encoding=0' in ln
                   for ln in tail):
                break

        _lib.mark(fh, 'post-run heap_caps — drift check')
        _lib.do(s, 'heap_caps', 2, fh)
        _lib.do(s, 'status', 2, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # Primary regression signal — pre-fix this appears literally every
    # single capture; post-fix it is 0.
    priv_buf_fails = len(re.findall(
        r'setup_dma_priv_buffer\(1206\): Failed to allocate priv',
        txt))

    # Two parallel signals — either the serial-cmd response OR the
    # spi_cam log line counts as "this capture happened". CDC TX
    # saturation during a concurrent encode can eat the response text
    # without affecting the underlying capture, so use whichever number
    # is higher.
    spi_pimslo_responses = max(
        len(re.findall(r'spi_pimslo capture_ms=\d+', txt)),
        len(re.findall(r'spi_cam: Capture all: \d/4 cameras', txt))
    )

    # DMA-internal drift across the 5 captures — both heap_caps lines
    # should be close. Big drop = we're leaking internal RAM per shot.
    dma_largest_vals = [int(m) for m in re.findall(
        r'heap_caps dma_int=\d+ \(largest=(\d+)\)', txt)]
    dma_drift = 0
    if len(dma_largest_vals) >= 2:
        dma_drift = dma_largest_vals[0] - dma_largest_vals[-1]

    extras = {
        'captures issued':   f'{N_CAPTURES}',
        'spi_pimslo replies': spi_pimslo_responses,
        'priv_buf fails':    f'{priv_buf_fails} (MUST be 0)',
        'dma largest start': dma_largest_vals[0] if dma_largest_vals else 'n/a',
        'dma largest end':   dma_largest_vals[-1] if len(dma_largest_vals) >= 2 else 'n/a',
        'dma drift':         f'{dma_drift} B (max {MAX_DMA_INT_DRIFT})',
    }
    _lib.print_summary('[13] SPI BACK-TO-BACK + FRAG CHECK', c, extras=extras)

    ok = (c['watchdogs'] == 0 and c['panics'] == 0 and
          c['ping_pong'] >= 1 and
          priv_buf_fails == 0 and
          spi_pimslo_responses >= N_CAPTURES and
          abs(dma_drift) <= MAX_DMA_INT_DRIFT)
    print(f"  VERDICT: {'PASS ✓' if ok else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
