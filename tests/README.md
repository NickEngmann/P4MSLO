# P4MSLO on-device tests

These are integration tests that drive the P4 over its serial command
interface (`factory_demo/main/app/app_serial_cmd.c`). They all assume
the P4 is flashed with the current firmware and enumerates at
`/dev/ttyACM0`.

Run the full suite after every flash — before hunting for regressions
elsewhere.

## Layout

- `tests/e2e/` — end-to-end tests. Each is a standalone Python script
  that opens a serial port, sends commands, captures the device log,
  and extracts a PASS/FAIL verdict. Each test writes its own log next
  to itself, e.g. `02_camera_capture_to_gif.log`.
- `tests/e2e/_lib.py` — shared helpers (open_port, do, drain, summarize).

## Running

```bash
# Full suite (stops on first failure)
tests/e2e/run_all.sh

# Single test
python3 tests/e2e/02_camera_capture_to_gif.py
```

## What each test guarantees

| # | Test | Guards against |
|---|---|---|
| 01 | `01_boot_and_liveness.py` | Firmware boots, LVGL task runs, ping→pong, cam_status replies for 4 slots |
| 02 | `02_camera_capture_to_gif.py` | Full UX: camera → photo_btn → navigate to ALBUM → bg encoder runs while user is on the gallery → `.p4ms` + `.gif` both land on disk → gallery playback resumes → up/down nav actually advances entries |
| 03 | `03_delete_modal.py` | Delete modal button semantics: encoder opens, NO default, up/down toggle, both encoder + menu confirm. Menu on closed modal exits to main. YES actually deletes gif + p4ms + preview. |
| 04 | `04_gallery_knob_nav.py` | btn_up / btn_down on the gallery actually change the viewed entry. Regression test for the "knob_rotation dispatcher had no UI_PAGE_GIFS branch" bug. |
| 05 | `05_bg_encode_while_on_gallery.py` | Background encoder encodes stale captures while the user sits on the gallery. Regression test for the "encode_should_defer included UI_PAGE_GIFS → encoder never ran" bug. |

## Adding a new test

1. Copy an existing test as a template.
2. Use `_lib.open_port()`, `_lib.do(s, cmd, wait, fh)`, `_lib.drain(s, dur, fh)`.
3. End with `_lib.print_summary(...)` + a clear PASS/FAIL verdict.
4. Exit `0` on pass, `1` on fail, so `run_all.sh` short-circuits.
5. Add it to `TESTS=` in `run_all.sh` and a row in the table above.

## Convention

- Tests are numbered by order of criticality — lowest numbers are the
  most foundational (boot + liveness). Higher numbers test specific
  features that depend on the foundation.
- Keep each test self-contained — no setup/teardown fixtures.
- Write the log output alongside the script; it stays in tree under
  `.gitignore` so we don't churn history but do preserve the last run.
