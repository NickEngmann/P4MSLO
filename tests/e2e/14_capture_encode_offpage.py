#!/usr/bin/env python3
"""PIMSLO capture → encode pipeline kickoff (heartbeat-light).

Part of the fast heartbeat (`run_fast.sh`). Verifies the user-facing
flow gets STARTED correctly without waiting for the full encode to
complete: photo_btn → SPI capture → save → queue encode → encoder
picks up job → .p4ms preview save begins.

Why not wait for the full .gif? On this board the photo_btn flow
runs the encoder via `pimslo_encode_queue_task` whose 16 KB stack
ends up in PSRAM (largest free internal block is ~7 KB even at
boot, so FreeRTOS's stack alloc falls back). Stack-in-PSRAM means
every push/pop is a 100-200 ns access, and Pass 2 takes ~55 s/frame
× 4 frames = ~5-7 minutes total. That's wildly over the 4-min
budget for the fast heartbeat. The full-encode verification is in
`run_all.sh` (which can spend the full encode time per test).

What this still catches:
  - photo_btn dispatch + visual flash + viewfinder free/realloc
  - SPI 4/4 capture
  - Save task fwrite of pos*.jpg (4 files) + P4 preview .jpg
  - `pimslo_encode_queue_task` actually picks up the queue
  - encode pipeline reaches the .p4ms direct-JPEG save (which means
    the JPEG decoder, PPA release, and tjpgd workspace are all wired
    up correctly)

Why MAIN instead of SETTINGS? `app_pimslo.c::encode_should_defer()`
treats both as safe pages. SETTINGS used to be the canonical "encoder
safe" page, but `menu_goto settings` sometimes drops between save-task
ESP_LOGI lines (CDC TX saturation during the per-pos save burst), so
the test would end up still on CAMERA at encode time and the encoder
defers forever. MAIN is one fewer hop and the dispatch reliably gets
through.

Pass criteria:
  - no panics, no watchdogs, no OOM
  - photo_btn succeeded (P4 photo saved)
  - spi_pimslo got ≥ 2 cams (encode path exercised)
  - "Encoding GIF from" log appears (encoder picked up the queue)
  - .p4ms preview saved (direct-JPEG path runs to completion)
"""
import os
import re
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)
# Wait long enough for the encoder to pick up the queue, get past
# its allocations, and emit the .p4ms direct-JPEG save log line. On
# this board that's ~25-30 s after the queue depth goes from 0 → 1
# (encode_queue_task blocks on xQueueReceive, then save_small_gif
# burns ~25 s decoding 4 × 2560×1920 JPEGs at scale=0). 60 s of
# headroom is plenty.
P4MS_WAIT_SECS = 90


def main():
    s = _lib.open_port()
    with open(LOG, 'w') as fh:
        _lib.drain(s, 2, fh)
        _lib.reset_state(s, fh, timeout=10)

        # Go to camera page and fire photo_btn. `take_and_save_photo`
        # + pimslo capture run here; pimslo_encode_queue_task queues
        # the encode job once all 4 SPI pos JPEGs land on SD.
        _lib.mark(fh, '1/4 capture on camera page')
        _lib.do(s, 'menu_goto camera', 4, fh)
        _lib.do(s, 'photo_btn', 2, fh)

        _lib.mark(fh, '2/4 wait for SPI capture to finish (up to 30 s)')
        t_end = time.time() + 30
        while time.time() < t_end:
            _lib.drain(s, 3, fh)
            with open(LOG) as lf:
                t = lf.read()
            if re.search(r'Capture \d+:.*usable=\d/4', t):
                break

        # Leave CAMERA for MAIN. With encode_should_defer's MAIN
        # carve-out (see app_pimslo.c), MAIN is encoder-safe — the
        # viewfinder PSRAM is freed inside the encode pipeline before
        # the 7 MB scaled_buf alloc, so there's no collision. Going to
        # MAIN instead of SETTINGS is ONE serial command instead of
        # two, which dodges the CDC-TX-saturation drop window during
        # the per-pos save burst.
        _lib.mark(fh, '3/4 leave camera for MAIN (encoder-safe page)')
        _lib.do(s, 'menu_goto main', 3, fh)

        _lib.mark(fh, f'4/4 wait for encoder kickoff + .p4ms save (up to {P4MS_WAIT_SECS} s)')
        t_end = time.time() + P4MS_WAIT_SECS
        while time.time() < t_end:
            _lib.drain(s, 3, fh)
            with open(LOG) as lf:
                t = lf.read()
            if 'Direct-JPEG .p4ms saved' in t:
                break
        _lib.do(s, 'status', 2, fh)
    s.close()

    with open(LOG) as f:
        txt = f.read()
    c = _lib.summarize(txt)

    # Capture count — ≥ 2 means encode path was exercised.
    capture_n = 0
    if c['captures']:
        try:
            capture_n = max(int(x) for x in c['captures'])
        except (ValueError, TypeError):
            capture_n = 0

    encoder_kicked = 'Encoding GIF from' in txt
    extras = {
        'capture cams':       f'{capture_n}/4',
        'encoder kicked':     'yes' if encoder_kicked else 'NO ← FAIL',
        'p4ms saved':         c['p4ms_saved'],
        'gifs saved':         f'{c["gifs_saved"]} (not awaited in fast suite)',
    }
    _lib.print_summary('[14] CAPTURE → ENCODE KICKOFF', c, extras=extras)

    ok = (c['watchdogs'] == 0 and c['panics'] == 0 and
          c['photo_btn'] == 1 and
          capture_n >= 2 and encoder_kicked and
          c['p4ms_saved'] >= 1)
    print(f"  VERDICT: {'PASS ✓' if ok else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
