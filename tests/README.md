# P4MSLO on-device tests

These are integration tests that drive the P4 over its serial command
interface (`factory_demo/main/app/app_serial_cmd.c`). They all assume
the P4 is flashed with the current firmware and enumerates at
`/dev/ttyACM0`.

The bar: **fast suite always green, full regression green before
commit.**

## Layout

- `tests/e2e/` — end-to-end tests. Each is a standalone Python script
  that opens a serial port, sends commands, captures the device log,
  and extracts a PASS/FAIL verdict. Each test writes its own log next
  to itself, e.g. `02_camera_capture_to_gif.log`.
- `tests/e2e/_lib.py` — shared helpers (`open_port`, `do`, `drain`,
  `summarize`, `reset_state`, `print_summary`). `_lib.drain` uses raw
  `select.select()` against the fd to bypass pyserial's read loop that
  has been observed to block for 14+ minutes when the USB CDC endpoint
  goes half-responsive.
- `tests/e2e/_*.py` — diagnostic scripts (not part of the suites).
  `_spi_20shot.py` is the one to run when you want to validate "does
  the SPI capture rig get N back-to-back 4/4 captures?"
- `tests/e2e/run_fast.sh` — heartbeat suite (3 tests, ~80 s).
- `tests/e2e/run_all.sh` — full regression (12 tests, ~10-15 min).

## Running

```bash
# ALWAYS run fast first — gates the full suite
tests/e2e/run_fast.sh

# Full regression (only if fast passes)
tests/e2e/run_all.sh

# Single test
python3 tests/e2e/02_camera_capture_to_gif.py

# 10 back-to-back SPI captures (diagnostic, not in either suite)
python3 tests/e2e/_spi_20shot.py 10
```

`run_all.sh` does a clean reboot before the first test so each run
starts from the same baseline heap state.

Set `P4MSLO_TEST_PORT=/dev/ttyACMn` if the P4 isn't at `/dev/ttyACM0`.

## Fast heartbeat (`run_fast.sh`, ~80 s)

| # | Test | What it verifies |
|---|------|------------------|
| 01 | `01_boot_and_liveness.py` | ping/status/cam_status respond; 0 panics, 0 watchdogs |
| 12 | `12_dma_heap_health.py`   | dma_int largest ≥ 2 KB, psram largest ≥ 8 MB (-64 B headroom for HEAP_POISONING_LIGHT canary), no `setup_dma_priv_buffer` failures, no `Failed to start BG worker`, no video_utils OOM |
| 11 | `11_heartbeat.py`         | Page nav over all 6 menu pages, buttons, one `spi_pimslo` capture, gallery entry + play, sd_ls, heap health, reset_state |

If fast suite fails, fix that before running anything else. A failing
fast test means foundational stuff is broken (boot, page nav, basic
heap state) — running the full suite would just produce 12 failures
all rooted in the same problem.

## Full regression (`run_all.sh`, 10-15 min)

Pre-commit bar. Order: cheap smoke first, slow bg observation last.
Each test gets a 420 s hard timeout via `timeout(1)` so a single
pyserial hang or firmware wedge can't hold the whole run hostage.
Suite stops on first failure.

```
01 → 12 → 10 → 02 → 13 → 06 → 08 → 03 → 07 → 04 → 09 → 05
```

| # | Test | What it verifies |
|---|------|------------------|
| 01 | `01_boot_and_liveness.py`    | (see fast suite) |
| 02 | `02_camera_capture_to_gif.py` | Full UX: camera → photo_btn → navigate to ALBUM → bg encoder runs while user is on the gallery → `.p4ms` + `.gif` both land on disk → gallery playback resumes → up/down nav advances entries |
| 03 | `03_delete_modal.py`         | Delete modal: encoder opens, NO default, up/down toggle, both encoder + menu confirm. Menu on closed modal exits to main. YES actually deletes gif + p4ms + preview. |
| 04 | `04_gallery_knob_nav.py`     | btn_up / btn_down on the gallery actually change the viewed entry. Regression test for the "knob_rotation dispatcher had no UI_PAGE_GIFS branch" bug. |
| 05 | `05_bg_encode_while_on_gallery.py` | Background encoder encodes stale captures while the user sits on the gallery. Regression test for the "encode_should_defer included UI_PAGE_GIFS → encoder never ran" bug. |
| 06 | `06_capture_timing.py`       | Per-window capture timing: W1 press→pimslo task ≤ 5 s, W4 viewfinder re-init ≤ 4.5 s, total ≤ 30 s. |
| 07 | `07_gallery_edge_cases.py`   | Gallery state transitions, empty/non-empty, repeated open/close, cmd queue health under churn. |
| 08 | `08_capture_edge_cases.py`   | First-entry camera page (no spurious "saving"), rapid photo_btn × 3, photo + immediate exit + re-enter, no-op enter+exit. **Note**: photo_btn count via CDC TX response is sensitive to encoder log saturation — see CLAUDE.md "Serial command USB-CDC TX saturation". |
| 09 | `09_gallery_empty_and_states.py` | Empty-album overlay, SD-not-detected overlay, scan rebuilds entries after encoding completes mid-test. |
| 10 | `10_album_open_from_main.py` | "ALBUM" menu item routes to UI_PAGE_GIFS (PIMSLO gallery), not the legacy P4-photo album. |
| 12 | `12_dma_heap_health.py`     | (see fast suite) |
| 13 | `13_spi_back_to_back.py`    | 5× `spi_pimslo` captures back-to-back, then drains to encoder idle before sampling drift. Asserts: 0 priv_buf alloc failures, 0 panics, 0 watchdogs, dma_int drift ≤ 2 KB. |
| 14 | `14_capture_encode_offpage.py` | photo_btn from main → encoder kickoff → wait for `.gif` to land. Validates the foreground encode path on `pimslo_encode_queue_task` end-to-end. |

Test 11 (`11_heartbeat.py`) is in the fast suite, not run_all.

## Diagnostic-only scripts (`_*.py`)

Not part of either suite. Run by hand when investigating something
specific.

| Script | Use |
|---|---|
| `_spi_20shot.py [N=20]` | Fires N back-to-back `spi_pimslo` captures, reports per-camera success rate. Goal: 20 consecutive 4/4. |
| `_isolate_encode.py` | Runs a single PIMSLO encode from existing `/sdcard/pimslo/pos*.jpg` files (uses the legacy `pimslo` serial cmd, not `photo_btn`). Useful when you want to time the encoder without going through SPI capture. |
| `_album_crash_aggressive.py`, `_album_crash_repro.py` | Older repros for the legacy album-page panic. Kept for archaeology. |
| `_o2_*.py` | Diagnostic snapshots for the `-O2` optimization investigation. Kept for archaeology. |

## Adding a new test

1. Copy an existing test as a template.
2. Use `_lib.open_port()`, `_lib.do(s, cmd, wait, fh)`,
   `_lib.drain(s, dur, fh)`. Always `_lib.reset_state(s, fh)` at the
   top — without it, prior-test heap/queue state leaks into yours.
3. End with `_lib.print_summary(...)` + a clear PASS/FAIL verdict.
4. Exit `0` on pass, `1` on fail, so `run_all.sh` short-circuits.
5. Add it to `TESTS=` in `run_all.sh` and a row in the table above.
6. Mark whether the test belongs in the fast suite or full
   regression. Fast suite is for foundational signals only — anything
   that reliably runs in < 30 s and tells you "is the firmware
   broken?"

## Convention

- Tests are numbered roughly by criticality and intended order — the
  lowest-numbered tests are the most foundational (boot + liveness).
  Higher numbers test specific features that depend on the foundation.
- Keep each test self-contained — no setup/teardown fixtures.
- Write the log output alongside the script; logs stay in tree under
  `.gitignore` so we don't churn history but do preserve the last run
  for inspection.
- For SPI / capture tests: gate on `0 priv_buf failures` and
  `0 panics` first, only then on captures-per-run counts. The capture
  count is sensitive to S3-side state (cameras occasionally need
  `cam_reboot all`); the firmware-side health metrics are what we
  actually defend.

## When tests fail intermittently

CLAUDE.md is the diagnosis log. The most common patterns:

- **`tlsf::remove_free_block` panic with garbage MTVAL** → was the
  PSRAM-stack overflow on `gif_bg`; resolved 2026-04-25 by moving
  the stack to BSS internal RAM. If it recurs, check whether another
  task ended up in PSRAM with a deep call chain.
- **`setup_dma_priv_buffer(1206)` SPI master panic** → dma_int pool
  starvation under concurrent capture+encode. The
  `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536` reservation is the
  buffer.
- **photo_btn count off-by-N in tests 08 / 13** → CDC TX saturation
  during encoder log spam dropped a response. Test 13 already has
  the dual-marker fallback; if you see this in 08, bump that test
  the same way.
- **SPI captures returning 0/4** → S3 cameras stuck in DATA mode.
  `cam_reboot all` is the fix; if that doesn't help, physical USB
  power-cycle of the S3 boards.
