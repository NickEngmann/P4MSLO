#!/usr/bin/env python3
"""Post-boot DMA-internal + PSRAM heap health check.

What broke this session (2026-04-24):

1. Adding a 32 KB `static uint8_t s_tjpgd_work[]` to app_gifs.c alongside
   show_jpeg's existing static dropped RETENT_RAM from 239 → 207 KiB.
   `video_utils_init` OOM'd at boot (ESP_ERR_NO_MEM 0x101) — viewfinder
   dead, user couldn't take a photo. The only serial symptom was
   `Failed to start BG worker` and an innocuous-looking video-stream
   init error.

2. Eagerly claiming the 4 KB SPI chunk-RX buffer at spi_camera_init
   silently failed when `heap_caps_aligned_alloc(64, 4096, DMA|INTERNAL)`
   was called after SD mount had already carved up the internal pool —
   first capture then hit `OOM for SPI chunk rx buffer (permanent 4096 B,
   largest DMA-internal=2432)` and returned 0/4 cameras.

Both regressions were invisible in existing tests because they only
surfaced on the CAPTURE path. This test asserts the relevant heap
thresholds right after boot, before any capture has run, so a future
regression fires on `run_all.sh` instead of being caught by the user
after another full debugging session.

Commands used:
  `heap_caps` → `dma_int=... (largest=...) int=... (largest=...)
                 psram=... (largest=...)`

Thresholds — if these start failing, either the heap budget genuinely
regressed or a new buffer is being claimed at init without justifying
itself in CLAUDE.md Known Issues."""
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)

# Minimum largest-free-block thresholds (bytes). These are POST-BOOT
# measurements — by the time the test talks to the device, the
# permanent 4 KB aligned chunk-RX buffer in spi_camera.c has already
# been claimed at spi_camera_init, so the remaining DMA-internal
# largest-free-block is naturally in the 3.5-4.5 KB range. We can't
# observe the pre-claim pool from a post-boot serial prompt — the
# more reliable signal is that no init-time alloc failure warnings
# fired, which this test also checks below.
#
# The threshold here is set to catch catastrophic fragmentation
# (largest dropping to a few hundred bytes from background task
# churn) rather than the already-absorbed claim itself.
MIN_DMA_INT_LARGEST = 2048
MIN_PSRAM_LARGEST   = 8 * 1024 * 1024


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 3, fh)
        _lib.do(s, 'ping', 2, fh)
        _lib.do(s, 'heap_caps', 3, fh)
        _lib.do(s, 'status', 2, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # Parse `heap_caps dma_int=X (largest=Y) int=X (largest=Y) psram=X (largest=Y)`
    m = re.search(
        r'heap_caps dma_int=(\d+) \(largest=(\d+)\) '
        r'int=(\d+) \(largest=(\d+)\) '
        r'psram=(\d+) \(largest=(\d+)\)',
        txt,
    )
    if not m:
        print('  ERROR: heap_caps output not parsed — firmware missing '
              'the extended heap_caps dump? See app_serial_cmd.c')
        _lib.print_summary('[12] DMA + HEAP HEALTH', c)
        print('  VERDICT: FAIL ✗')
        sys.exit(1)

    dma_int       = int(m.group(1))
    dma_largest   = int(m.group(2))
    int_free      = int(m.group(3))
    int_largest   = int(m.group(4))
    psram_free    = int(m.group(5))
    psram_largest = int(m.group(6))

    # Init-time warnings that indicate a claimed buffer didn't get its
    # memory — both are catastrophic for the capture pipeline but the
    # device keeps booting, so they're easy to miss on manual runs.
    scratch_fail = 'SPI scratch alloc failed' in txt
    chunk_fail   = 'SPI chunk rx eager alloc failed' in txt
    bgworker_fail = 'Failed to start BG worker' in txt

    # video_utils_init OOM symptoms (the "stash pop broke the viewfinder"
    # regression from this session). 0x101 = ESP_ERR_NO_MEM.
    video_oom = bool(re.search(r'video.*0x101|video_utils.*failed', txt, re.I))

    extras = {
        'dma_int largest': f'{dma_largest} B (min {MIN_DMA_INT_LARGEST})',
        'int largest':     f'{int_largest} B',
        'psram largest':   f'{psram_largest} B (min {MIN_PSRAM_LARGEST})',
        'scratch alloc fail':  'yes ← FAIL' if scratch_fail else 'no',
        'chunk alloc fail':    'yes ← FAIL' if chunk_fail else 'no',
        'bg worker fail':      'yes ← FAIL' if bgworker_fail else 'no',
        'video utils OOM':     'yes ← FAIL' if video_oom else 'no',
    }
    _lib.print_summary('[12] DMA + HEAP HEALTH', c, extras=extras)

    ok = (c['watchdogs'] == 0 and c['panics'] == 0 and
          c['ping_pong'] >= 1 and
          dma_largest >= MIN_DMA_INT_LARGEST and
          psram_largest >= MIN_PSRAM_LARGEST and
          not scratch_fail and not chunk_fail and
          not bgworker_fail and not video_oom)
    print(f"  VERDICT: {'PASS ✓' if ok else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
