# P4MSLO — Implementation Phase Plan

This is the working plan for the current multi-feature session. Features are grouped into 5 phases, each landing as its own commit. Phases are ordered so low-risk changes ship first and each later phase can assume the earlier ones are in place.

## Lessons-learned index (updated 2026-04-21)

Concrete things this multi-session implementation cycle taught me, all rediscovered via on-device testing after I'd already "verified" them via host-test + build-only checks:

1. **Host tests + a clean build are not verification.** They guarantee the code compiles and the pure-logic parts work. They tell you nothing about what a 4 KB FreeRTOS task stack can survive, about PSRAM fragmentation under real workloads, or about GIF decoder behavior when the encoder is holding 7 MB. You have to actually flash the firmware and drive the UI to find the interesting failures.

2. **PSRAM is not a flat pool of contiguous bytes.** 7.7 MB free ≠ 7.7 MB allocatable. After a few camera on/off cycles the largest contiguous block drops to ~6.5–6.8 MB — right at the PIMSLO GIF decoder's footprint. Any new 7+ MB consumer must either pre-allocate at boot (before fragmentation) or be rewritten to not need a 7 MB block at all.

3. **Stack sizes matter for callbacks.** The video-stream task runs with 4 KB. A 2 KB stack buffer in any function it calls will blow the stack. Symptom: `MCAUSE=0x1b`, panic in `__ssprint_r`, `S0/FP=0x00000000`. Fix: heap-allocate the buffer or move the work to a different task with more stack.

4. **Every page redirect should explicitly manage its PSRAM footprint.** A one-size-fits-all "leaving_main" hook that unconditionally reallocates viewfinder buffers hurts pure-display pages (GIFS, ALBUM) that need that PSRAM for their own decoders.

5. **GIF playback holds memory across navigation.** If you navigate away without calling `app_gifs_stop()`, the decode buffer + canvas stays allocated and cripples the next camera page's startup.

6. **Global-pointer-based subsystems need NULL guards at every public entry point.** AI detection is off by default to save PSRAM; the album page still unconditionally calls its COCO detector. Result: NULL-this panic. Fix: the detector's public entry point checks initialized state.

7. **The SPI 0x01–0x07 "safe range" was conservative.** With the 10× burst retry the master already does, any command in 0x01–0x0F is reliable. Expanded command space to 0x08 (AUTOFOCUS) and 0x09 (SET_EXPOSURE) without issue.

8. **Extended IDLE header is backward-compatible.** Masters still reading only 5 bytes keep working unchanged — the extra AE bytes at offsets 5-9 are just zero-padding to them.

9. **Background encode vs foreground playback is the architecture issue.** The GIF encoder needs 7 MB; the player needs 7 MB; PSRAM only has ~8 MB contiguous. Neither pausing encoding nor defragmenting solves this robustly — only a streaming decoder that doesn't need the full-res intermediate does. Tracked as "Phase 3-style refactor" in the next steps section.

---

## On-device verification summary (2026-04-21)

After the code landed, flashed to real P4-EYE hardware and 2 S3 cameras (OTA via HTTP). Reproduced the originally-reported "take picture → P4 freezes + returns to home screen" crash on the first press after flash — traced via RISC-V panic dump (`MCAUSE=0x1b`, stack protection fault in video-stream task), root-caused to a 2 KB on-stack copy buffer in the Phase 2 preview-save path, fixed by moving the save into the PIMSLO capture task (8 KB stack) + heap-allocating the buffer. Commit `988da32`.

Second issue surfaced: GIF encoder OOM because the capture task reallocated viewfinder PSRAM too eagerly, starving the follow-on GIF task. Fixed with a handoff flag that keeps viewfinder freed through the full capture+encode cycle (same commit).

Third issue: gallery entry OOM'd the GIF decoder because my Phase 2 `leaving_main()` always reallocated camera buffers — even for pure-display pages. Fixed by making each page explicitly state whether it needs the viewfinder. Commit `a53201d`.

End-to-end verified on device: photo button → P4 JPEG saved → preview JPG copied → SPI capture 3/4 cameras → GIF queued → GIF encoded to `P4M0008.gif` (7.9 MB on SD), zero panics. 80/80 host tests pass, +20 from this session.

Phase 5 `wifi_start` runs, fails cleanly (C6 SDIO transport pins not wired to esp_hosted defaults), device stays stable — confirms the documented "needs schematic review" blocker.

### Round 2 on-device findings + fixes (2026-04-21 later)

After the user pointed out that the "end-to-end" claim was too strong ("GIF menu still says 'press to play', Album crashes"), I ran an exhaustive page+button sweep via serial. Found:

- **Gallery "press to play" bug** — GIFS decode_buffer (6.7 MB) alloc failed because the GIF decoder also kept a 3.3 MB pixel_indices buffer alongside it; 10 MB > 8 MB max contig.
  Fix: in-place backwards palette→RGB565 conversion, single buffer used for both LZW output and final RGB565. Peak 10 MB → 6.7 MB. Commit `e0dea27`.
- **Album crash** — `app_coco_od_detect()` called `detect->run(img)` on a NULL `detect` because AI init is off by default at boot.
  Fix: guard the public entry point against uninitialized state. Commit `e0dea27`.
- **Camera-buffer realloc fails after GIF→CAMERA** — GIF playback kept running after leaving GIFS, holding 7 MB PSRAM.
  Fix: `app_gifs_stop()` on every page transition (in `leaving_main` and `redirect_to_main_page`). Commit `8666f6d`.
- **PSRAM fragmentation after camera ops** — contiguous block drops below decode-buffer size even with 7.7 MB total free.
  Mitigation: video-stream buffer alloc/free churn on OOM to consolidate heap. Commit `06f4ead`.
- **Encoder/player contention** — PIMSLO GIF encode holds 7 MB, gallery needs another 7 MB.
  Mitigation: skip auto-play when encoder is busy. Commit `06f4ead`.

53+ page/button combinations now pass a clean sweep. Real remaining issue: full-res 1824×1920 PIMSLO GIF playback is at the PSRAM ceiling and occasionally OOMs under fragmentation + encoder contention. Decided next: streaming decoder rewrite to remove the 7 MB decode buffer entirely.

---


## Features in this session

| # | Feature | Phase | Complexity | Risk |
|---|---|---|---|---|
| D | Firmware version + git SHA on boot + status API | 1 | Trivial | None |
| F2 | Home screen: kill viewfinder, plain **white** background | 2 | Medium | Medium |
| F1 | GIF gallery auto-plays on entry (verify; likely no-op) | 2 | Low | Low |
| F3 | P4 photo becomes preview placeholder for in-flight GIFs | 2 | Medium | Low |
| E | Parallel GIF encoding (decode N+1 while encoding N) | 3 | High | Medium |
| B | Cross-camera exposure sync before PIMSLO burst | 4 | Medium | Low |
| C | OV5640 autofocus before PIMSLO burst | 4 | Medium | Medium |
| A | P4 OTA via onboard ESP32-C6 (MVP scope) | 5 | High | Highest |

Design decisions (settled):

1. **Home-screen background = white** (per user)
2. **P4 preview photo kept permanently** after GIF ships (archive / backup value)
3. **AF fires every PIMSLO burst by default**, with a Settings toggle ("Fast capture mode") that skips it
4. **P4 OTA scope = MVP**: OTA endpoint that accepts firmware.bin and reboots. No live-log-streaming, no remote shell — those can be follow-on work later.

---

## ~~Phase 1 — Firmware version + git SHA~~ ✅ DONE

**Goal**: every build knows what it is. Visible on boot, exposed in the `/status` API.

### ~~Tasks~~
- ~~Root `CMakeLists.txt`: `git describe --always --dirty=+ --tags` → `-DP4MSLO_FIRMWARE_VERSION="..."`~~ ✓
- ~~Supports docker-build path via `P4MSLO_FIRMWARE_VERSION` env var fallback~~ ✓
- ~~PIO pre-build script `esp32s3/scripts/inject_version.py` does the same for S3~~ ✓
- ~~P4 boot log prints the version at app_main entry~~ ✓
- ~~S3 boot log appends version to existing splash~~ ✓
- ~~P4 serial `status` command includes `fw=<version>` prefix~~ ✓
- ~~S3 HTTP `/api/v1/status` JSON adds `firmware_version` field~~ ✓

### Verified
- P4 serial reports: `CMD>fw=d5da08d+ page=MAIN sd=yes ...`
- Binary contains version string (`strings firmware.bin | grep d5da08d`)

---

## ~~Phase 2 — Home-screen & gallery UX~~ ✅ DONE (with follow-up noted)

**Goal**: free PSRAM by killing the home-screen viewfinder, and make the gallery show something meaningful immediately after a capture.

### ~~F2 — Home screen becomes white, viewfinder disabled~~ ✅

The current home (`UI_PAGE_MAIN`) is running the P4 camera feed behind the LVGL UI. That eats PSRAM and CPU. We want the home screen to be a plain white LVGL background when the P4 is idle.

Tasks:
- Find the viewfinder task/callback that pushes camera frames to the main page's canvas
- When entering `UI_PAGE_MAIN`: stop the viewfinder, release the camera frame buffers (`app_video_stream_free_buffers()` if exposed, or similar), set the page's background to `LV_COLOR_WHITE`
- When leaving `UI_PAGE_MAIN` (entering camera/preview pages): restart viewfinder
- Measure PSRAM before/after: expect ≥7MB freed (the camera framebuffer)
- Document the PSRAM win — it's what unlocks Phase 3

### ~~F1 — Gallery auto-plays on entry~~ ✅

User reports the forward/back/menu nav works correctly; only the auto-play-on-entry is missing. Task:
- Inspect `ui_extra_redirect_to_gifs_page()` → verify entry state
- If the page enters in a paused state, trigger play immediately after load
- If it already auto-plays (user thinks it might), confirm and note as no-op

### ~~F3 — P4 photo = GIF preview placeholder~~ ✅ (gallery JPG-display path DEFERRED)

When the photo button is pressed, the P4 already takes its own photo via its camera pipeline AND triggers the SPI cameras AND queues a GIF encode. The user wants the P4 photo to appear in the gallery as the preview for that pending GIF until the encode finishes.

Filename scheme (proposed):
- PIMSLO captures get a numeric index (e.g., `P4M0001`) via the existing `app_pimslo` NVS counter
- The P4 photo taken with that same button press is saved as `/sdcard/p4mslo_previews/P4M0001.jpg` (same index)
- The full GIF ends up at `/sdcard/p4mslo_gifs/P4M0001.gif`
- Gallery scanner prefers `.gif` if it exists, falls back to `.jpg` preview if not
- Preview `.jpg` is **kept permanently** after GIF finishes (user-chosen)

Tasks:
- Modify P4 photo save path so button-press photos go to `p4mslo_previews/` with the PIMSLO index
- Extend `app_album` or `app_gifs_scan` to include `.jpg` previews for entries that don't yet have a matching `.gif`
- When the gallery shows a `.jpg` preview (not yet-encoded), mark it visually (e.g., "⏳ encoding" badge or dimmed thumbnail)
- When a new GIF encode finishes, re-scan gallery so the preview swaps to the animated GIF

### Files expected to change
- UI layer for home-page background + viewfinder state (`factory_demo/main/ui/ui_extra.c`, possibly generated `ui_*.c` files for Panel config)
- `factory_demo/main/app/Video/app_video_stream.c` for start/stop plumbing
- `factory_demo/main/app/Video/app_video_photo.c` for save path / filename
- `factory_demo/main/app/Pimslo/app_pimslo.c` to wire the photo index ↔ capture index
- `factory_demo/main/app/app_album.c` for preview-file awareness
- `factory_demo/main/app/Gif/app_gifs.c` for post-encode re-scan

### Verification
- Boot → home screen is plain white, no viewfinder
- `status` command reports ≥7MB more free PSRAM than before
- Press photo button → gallery shows P4 photo immediately
- Wait ~50s → gallery now shows the animated GIF (replace)
- Both files still exist on SD (preview `.jpg` kept)

---

## Gallery UX + streaming GIF decoder ✅ SHIPPED (2026-04-21)

After the user pointed out that gallery playback was stuck at ~2 s/frame and demanded real fixes rather than deferrals, landed a substantially rewritten GIF playback path. Combines:

- **Streaming-ish decoder** that drops the 6.7 MB full-res RGB565 intermediate — `gif_decoder_next_frame()` now nearest-neighbor-scales palette indices straight into a 240×240 target buffer. Peak memory 10 MB → ~3.6 MB (3.5 MB `pixel_indices` + 115 KB canvas).
- **Two-step decode API** (`read_next_frame` / `decode_read_frame` / `discard_read_frame`) so the app layer can peek a frame's hash before committing to a full LZW decode. Hash is FNV-1a over the compressed LZW bytes, so reverse frames in PIMSLO palindromes fingerprint identical to their forward originals.
- **Per-decoder frame-offset map** — after the first pass through a file, each frame's (start_offset, end_offset, hash, delay) is remembered. On subsequent loops the decoder fast-paths to `fseek(end_offset)` and returns the recorded hash without reading the ~1 MB of compressed data again. Measured result: hits the GIF's native 150 ms framerate on loop 2 and beyond; first loop is still decode-limited (~600-700 ms per unique frame).
- **Canvas frame cache** in `app_gifs.c` — 115 KB per unique frame, keyed on the decoder's hash. A 6-frame PIMSLO palindrome dedups to 4 unique canvases (~460 KB total).
- **JPEG preview fallback** — gallery scans both `/sdcard/p4mslo_gifs/*.gif` and `/sdcard/p4mslo_previews/*.jpg`. When a capture's GIF encode isn't done yet, the gallery shows its static JPEG via tjpgd, overlaid with a centered **PROCESSING** badge so the user knows the animated version is still in flight. As soon as the GIF lands on disk the next gallery scan prefers it.
- **Auto-play on nav** — `btn_up` / `btn_down` open and play the neighboring entry immediately. No more "press to play" intermediate state.
- **On-screen entry name** — bottom-center LVGL label (not drawn onto the canvas pixel buffer, which caused a screen-blank bug on an earlier attempt) shows the current filename.

Verified on device: fresh-boot GIF entry, post-fragmentation GIF entry, encoder-busy entry (gracefully static-frames), JPEG-preview navigation (shows PROCESSING badge), 20-step full-gallery navigation without panics, steady-state ~150 ms framerate after first loop.

---

## Phase 3 — Parallel GIF encoding 🟡 DEFERRED (see note)

**Deferral rationale (2026-04-19):** After landing Phase 2, the measured MAIN-page PSRAM state is 8.3 MB free, which is also the largest-contiguous-block ceiling (per CLAUDE.md "Known Issues" — 32 MB total, ~8.26 MB max contiguous after boot). The planned double-buffered inter-frame pipeline needs 2× 7.0 MB RGB565 scaled buffers = 14 MB peak — physically infeasible without freeing substantially more PSRAM than what Phase 2 unlocked.

Alternative considered: **intra-frame dither||LZW pipelining** (split the pass-2 hot loop across cores using a row-level ring buffer, no extra large PSRAM). Theoretical gain ~20–25% of encode time, but requires a deep refactor of a tightly-tuned inner loop with per-row cross-core synchronization. Not worth the risk right now.

Also, the remaining user-visible pain is the **capture** latency (~3.4 s) not the **encode** latency (~50 s in the background) — Phase 4's autofocus + exposure-sync improves every frame's image quality, a more visible win per unit of effort.

Revisit Phase 3 if/when: (a) the LCD/LVGL framebuffer is relocated or shrunk, (b) the bottleneck shifts to encoding (e.g. video recording), or (c) profiling shows the dither+LZW split would actually beat its sync overhead.

### ~~Original plan~~

**Goal**: cut GIF encode time roughly in half by pipelining decode + encode.

### Current pipeline (serial)

```
for each of 4 source JPEGs:
  decode JPEG → RGB565 buffer   (~2s)
  feed RGB to encoder quantize  (~1s)
encoder palette-build
for each of 6 output frames:
  decode JPEG → RGB565 buffer   (~2s)
  encoder.encode_frame(RGB)     (~3s)
```

Total ≈ 50s for a 6-frame GIF.

### Target pipeline (double-buffered)

```
Core 0 task:    decode frame N+1 into buffer_B
Core 1 task:    encode frame N   from buffer_A
Once both done: swap A↔B, advance N
```

Peak PSRAM usage goes from 1× 7MB RGB buffer to 2× 7MB = 14MB. This is feasible **only because Phase 2 freed the viewfinder buffers**.

### Tasks
- Refactor `app_gifs.c` pipeline loop to two tasks + semaphores for handoff
- Pre-allocate both RGB565 buffers at pipeline start (reject with graceful error if PSRAM not available)
- Benchmark: time a single `spi_pimslo` call end-to-end before and after
- Goal: ≥30% reduction in GIF encode time (50s → ~35s or better)

### Files expected to change
- `factory_demo/main/app/Gif/app_gifs.c` (core refactor)
- Possibly `factory_demo/main/app/Gif/gif_encoder.c` if the encoder's internal state machine needs to be split

### Verification
- Time before: run 3 `spi_pimslo`, note GIF encode times
- Time after: same test, compare
- Verify GIF output is still visually identical (no corruption from the refactor)

---

## Phase 4 — Image quality: exposure sync + autofocus 🟨 PARTIAL (AF stubbed)

**Shipped:**
- ~~SPI command space extended (0x08 AUTOFOCUS, 0x09 SET_EXPOSURE) + 0x08 AF_LOCKED status bit~~ ✓
- ~~Extended IDLE header with AE gain (bytes 5-6) + exposure (bytes 7-9) — sent on every status poll~~ ✓
- ~~`CameraManager::getExposure`/`setExposure`/`setAutoExposure` via OV5640 regs 0x350A/B and 0x3500-0x3502~~ ✓
- ~~`CameraManager::autofocus()` stub (returns true immediately — real AF deferred; see below)~~ ✓
- ~~Master helpers: `spi_camera_read_exposure`, `spi_camera_set_exposure`, `spi_camera_sync_exposure`, `spi_camera_autofocus_all`~~ ✓
- ~~`app_pimslo` capture task calls AF + exposure-sync pre-trigger (skipped in fast mode)~~ ✓
- ~~NVS-backed "fast capture mode" + `fast_capture on|off|status` serial command~~ ✓
- ~~Serial commands: `cam_ae [N]`, `cam_sync_ae [ref]`, `cam_af`, extended `cam_status` with AF bit~~ ✓
- ~~SPI control callback extended to carry payload bytes (needed for SET_EXPOSURE)~~ ✓

**Deferred — OV5640 autofocus firmware blob**
OV5640 AF needs a ~4KB firmware blob loaded to sensor register 0x8000 via SCCB before AF commands work. esp32-camera v2.0.4 does NOT bundle this blob — the sensor driver has register access but no AF FSM.

To unstub `CameraManager::autofocus()`:
1. Source the OV5640 AF firmware (commonly distributed as `OV5640_AF.bin`, ~4KB — e.g. search in ArduCAM/Seeed projects).
2. Embed as C array in `esp32s3/src/camera/ov5640_af_fw.h`.
3. On `CameraManager::begin()`, if PID==0x5640: write the blob via `sensor->set_reg`/SCCB write-multi starting at 0x8000, then set 0x3022=0x08 to start firmware, poll 0x3029 until == 0x70 (idle).
4. In `autofocus()`: write 0x3022=0x03 (single AF trigger), poll 0x3029 for focus state, return true when `0x3029 == 0x10`.

This is a focused ~100-line addition once the blob is obtained. SPI wire protocol + master-side polling + AF_LOCKED status bit are already in place, so only the CameraManager body changes.

**Deferred — UI Settings toggle for fast capture mode**
SquareLine Studio generates the settings panel; adding a toggle there means either regenerating from the SquareLine project file or synthesizing a custom LVGL switch in `ui_extra.c` and splicing it into the existing rotate list. Currently toggleable only via serial `fast_capture on|off`. UI follow-up.

### Verified
- P4 ESP-IDF build: compiles clean, factory_demo.bin 2.45MB (77% partition free)
- S3 PlatformIO build: compiles clean, 51.9% flash used
- 60/60 host tests pass

### ~~Original plan~~

**Goal**: improve PIMSLO output quality by making the 4 cameras agree on exposure and focus before firing.

### B — Cross-camera exposure sync

Right now each OV5640 runs its own auto-exposure loop independently. For a 4-way stereoscopic shot, this means each frame has different brightness → visible "flicker" in the oscillating GIF.

Protocol:
- New SPI command `SPI_CMD_READ_EXPOSURE = 0x08` — slave responds with current AE gain + integration time
- New SPI command `SPI_CMD_SET_EXPOSURE = 0x09` — slave applies explicit values
- Master flow (before PIMSLO burst):
  1. Pick reference camera (e.g., camera 2 — our most-reliable one)
  2. Send `SPI_CMD_READ_EXPOSURE` to reference → get 4 bytes of AE parameters
  3. Broadcast `SPI_CMD_SET_EXPOSURE` with those values to the other 3 cameras
  4. Wait ~50ms for AE to settle
  5. Fire GPIO34 trigger

### C — OV5640 autofocus

OV5640 has built-in AF — currently unused. Firing AF before every burst takes ~500-800ms but dramatically improves sharpness for non-infinity subjects.

Protocol:
- New SPI command `SPI_CMD_AUTOFOCUS = 0x0A` — slave triggers AF, replies immediately, continues AF in background
- New status flag `SPI_STATUS_AF_LOCKED = 0x08` (bit 3 in status byte) — master polls until all 4 cameras have AF locked
- Master flow:
  1. Broadcast `SPI_CMD_AUTOFOCUS` to all 4 cameras (they start AF in parallel)
  2. Poll each camera's status until `AF_LOCKED` bit is set
  3. Timeout after 2s — proceed with best-effort focus if some cams don't lock
  4. Do exposure sync (B)
  5. Fire trigger

AF firmware blob:
- OV5640 AF requires a firmware blob (a few KB) loaded to the sensor via I2C
- esp32-camera v2.0.4 may or may not include this — need to verify and add if missing
- Source: [Espressif esp32-camera](https://github.com/espressif/esp32-camera) has the blob somewhere; worst case, extract from Arducam examples

Settings toggle (per user):
- New NVS key `fast_capture_mode` (bool, default false)
- Settings page adds a toggle "Fast capture (skip autofocus)"
- When enabled, Phase 4 AF step is skipped → ~800ms faster capture

### Files expected to change
- `esp32s3/src/camera/CameraManager.cpp` — add `setExposure()`, `getExposure()`, `autofocus()` methods
- `esp32s3/src/spi/SPISlave.h` — new command constants, new status flag
- `esp32s3/src/spi/SPISlave.cpp` — command handler
- `esp32s3/src/main.cpp` — route new control commands to CameraManager
- `factory_demo/main/app/Spi/spi_camera.c` — `spi_camera_read_exposure()`, `spi_camera_set_exposure()`, `spi_camera_autofocus_all()`
- `factory_demo/main/app/Pimslo/app_pimslo.c` — insert the AF + exposure-sync steps before `spi_camera_capture_all()`
- UI: Settings page for the fast-capture toggle

### Verification
- Take same scene with feature off vs on: all 4 JPEGs should have similar brightness (within ~10%) with feature on, vs varying noticeably with it off
- Subjects at 50cm should be noticeably sharper with AF
- Fast-mode toggle: capture time drops by ~800ms when enabled

---

## Phase 5 — P4 OTA via ESP32-C6 (MVP) 🟨 PARTIAL (code shipped, on-device WiFi unverified)

**Shipped:**
- ~~`espressif/esp_wifi_remote` + auto-resolved `esp_hosted` + `wifi_remote_over_eppp` managed components added (target=esp32p4)~~ ✓
- ~~`espressif/mdns` managed component added~~ ✓
- ~~`factory_demo/main/wifi_config.h` with hardcoded creds matching the PIMSLO cameras' "The Garden" SSID~~ ✓
- ~~`factory_demo/main/app/Net/app_p4_net.[ch]` — C6 enable via BSP_C6_EN_PIN, WiFi STA init, event-driven DHCP, mDNS (`pimslo-p4.local`), and HTTP server~~ ✓
- ~~HTTP endpoints: `GET /api/v1/status` (fw version, uptime, free heap, IP) and `POST /api/v1/ota/upload` (accepts firmware.bin, writes to OTA partition, reboots)~~ ✓
- ~~Serial commands: `wifi_start`, `wifi_stop`, `wifi_status`~~ ✓
- ~~WiFi is OFF by default at boot — zero regression risk to the SPI capture flow until user explicitly runs `wifi_start` (same opt-in pattern we use on the S3 cameras with `DISABLE_WIFI=1`)~~ ✓

**Deferred — on-device WiFi connection hasn't been verified**
The CMake build resolves cleanly, but actually *associating to WiFi* requires the esp_hosted transport (SPI or SDIO) between the P4 and the onboard C6 to be configured for the right pins. The current `esp32_p4_eye` BSP defines ONLY `BSP_C6_EN_PIN` (GPIO9) — the SDIO/SPI data pins for coprocessor comms are not exposed by the BSP and likely need verification against the P4-EYE schematic.

Unblock path once someone can look at the schematic or run the test board:
1. Flash the committed firmware and run `wifi_start` via serial.
2. Watch for `[p4_net] got IP: 192.168.x.y` in the log. If yes — we're done.
3. If `esp_wifi_init` fails or connection times out, look at `sdkconfig` → "Wi-Fi Remote over EPPP" → transport type (UART vs SPI vs SDIO) and pin numbers, and match them to the P4-EYE schematic. Likely candidates for SPI transport: MOSI/MISO/SCLK/CS/INTR on pins currently used by ESP-HOSTED defaults (MOSI=23/MISO=19/SCLK=18/CS=5/INTR=17 per `wifi_remote_over_eppp/Kconfig`).
4. Separately verify that the C6 has `esp_hosted-slave` firmware on it. Fresh dev boards often ship with a placeholder C6 image — if WiFi init works but scan returns nothing, the C6 side needs its own slave firmware flash.
5. After WiFi works, re-run the 4-camera SPI capture test to confirm WiFi doesn't destabilize the shared bus. If it does, revisit the mitigation plan in the original Phase 5 notes (TX power, power-save disable, or button-gated activation).

**Deferred — "live log streaming" and "remote shell"** endpoints as per original MVP scope decision.

### Verified (what CI-equivalent checks caught)
- P4 ESP-IDF build: compiles clean, factory_demo.bin 2.94 MB (72% partition free, +180 KB for WiFi/HTTP/OTA/mDNS)
- 60/60 host tests pass
- No project-local warnings introduced (only pre-existing `compute_box_stats defined but not used` in gif_quantize.c)
- Runtime WiFi connection: UNVERIFIED (requires hardware + schematic confirmation)

### ~~Original plan~~

**Goal**: OTA-update the P4 firmware without plugging in USB.

### MVP scope (per user)

In:
- Hardcoded WiFi credentials
- `/api/v1/ota/upload` endpoint on the P4 that accepts firmware.bin and triggers reboot
- `/api/v1/status` endpoint reporting P4 firmware version, uptime, free heap
- mDNS name (`pimslo-p4.local`) for easy discovery

Out (for now):
- Serial-log streaming over WiFi
- Remote shell / command execution
- Status dashboards / telemetry

### Architecture

The P4 does NOT have its own WiFi — the onboard ESP32-C6 is its coprocessor via `esp_hosted`. Steps:

1. **Verify C6 firmware state**
   - Is the P4-EYE shipping with esp_hosted-slave firmware pre-loaded on the C6?
   - If not, we need to flash the C6 too — research the C6's USB / SPI flashing path on the P4-EYE (it may route via the P4's JTAG/USB)
2. **Enable esp_hosted on P4 side**
   - Add `espressif/esp_hosted` managed component to `factory_demo/CMakeLists.txt`
   - Configure `sdkconfig` for esp_hosted master (SDIO pins, chip = ESP32-C6, speed)
   - Connect esp_hosted's virtual netif to standard `esp_netif` / `esp_wifi_*` API
3. **Bring up WiFi STA on P4**
   - Hardcode SSID/password in a new `factory_demo/main/wifi_config.h`
   - Call `esp_wifi_init(); esp_wifi_start(); esp_wifi_connect();` on boot — or wait for a trigger (safer: hold a button at boot)
4. **HTTP server with OTA endpoint**
   - Standard ESP-IDF `esp_http_server` + `esp_https_ota` (or `esp_ota_begin/write/end` if we do it manually)
   - Accept firmware binary as request body
   - Validate it's a real app image, write to inactive partition, set boot partition, reboot
5. **mDNS discovery**
   - `mdns_init(); mdns_hostname_set("pimslo-p4");`
   - User can then `curl http://pimslo-p4.local/api/v1/ota/upload ...` from any LAN host

### Risk: does P4 WiFi destabilize SPI?

On the S3s, WiFi caused SPI flakiness which we solved by `DISABLE_WIFI=1`. The P4's WiFi goes through a separate chip (C6) over SDIO — totally different timing, different RF chain, probably different coexistence behavior. But we won't know until we test.

Mitigation plan if it does destabilize SPI:
- Treat P4 WiFi like we treat S3 WiFi — it's OFF by default, turn on via button-press or OTA-request command
- Hardware separation of SDIO from camera SPI traces (already true physically on the P4-EYE)
- Tune WiFi TX power down, disable power-save (same tricks we documented for S3)

### Files expected to change
- `factory_demo/CMakeLists.txt` — add esp_hosted component
- `factory_demo/sdkconfig.defaults` — enable esp_hosted + WiFi
- `factory_demo/main/wifi_config.h` (NEW) — hardcoded creds
- `factory_demo/main/main.c` — WiFi + HTTP + OTA setup
- `factory_demo/main/app/app_control.c` — new control command to enable/disable P4 WiFi on demand
- New: `factory_demo/main/app/p4_http_server.c` (or similar)

### Verification
- P4 boots, shows its IP on the LCD or serial log
- From host: `ping pimslo-p4.local` resolves
- From host: `curl http://pimslo-p4.local/api/v1/status` returns JSON with firmware version
- From host: `curl -X POST http://pimslo-p4.local/api/v1/ota/upload --data-binary @factory_demo.bin` — firmware updates, P4 reboots, new version shown on LCD
- **Regression test**: after P4 WiFi is active, run 5× `spi_capture_all` — expect same 100% first-try rate we have now. Record any flakiness.

---

## Commit strategy

One commit per phase. Each commit is self-contained, has its own verification evidence in the commit message, and can be reverted independently if it breaks something.

```
commit A  Phase 1  docs+infra: firmware version on boot + in /status
commit B  Phase 2  ui: white home screen + P4 photo as GIF preview
commit C  Phase 3  perf: parallel GIF encoding (N decode || N-1 encode)
commit D  Phase 4  quality: exposure sync + autofocus + fast-capture toggle
commit E  Phase 5  feat: P4 OTA via onboard ESP32-C6 (MVP)
```

## Known unknowns / blocker candidates

Tracked here so we don't forget:

1. **OV5640 AF firmware blob availability** — may require integrating an external AF binary into esp32-camera patch; could add hours or be a no-op
2. **C6 firmware state on the P4-EYE out-of-box** — if not esp_hosted-ready, we need a flashing path
3. **esp_hosted SDIO pinout on P4-EYE** — need schematic / BSP definition
4. **Parallel GIF encode PSRAM peak** — actual concurrent allocation might exceed 14MB depending on how `heap_caps_malloc` allocates; worst case we fall back to single-buffer (no perf gain, but no regression)
5. **UI page-enter hook for gallery** — may need to add an on-enter callback if one doesn't exist
