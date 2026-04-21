# P4MSLO — Implementation Phase Plan

This is the working plan for the current multi-feature session. Features are grouped into 5 phases, each landing as its own commit. Phases are ordered so low-risk changes ship first and each later phase can assume the earlier ones are in place.

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

## Phase 2 — Home-screen & gallery UX

**Goal**: free PSRAM by killing the home-screen viewfinder, and make the gallery show something meaningful immediately after a capture.

### F2 — Home screen becomes white, viewfinder disabled

The current home (`UI_PAGE_MAIN`) is running the P4 camera feed behind the LVGL UI. That eats PSRAM and CPU. We want the home screen to be a plain white LVGL background when the P4 is idle.

Tasks:
- Find the viewfinder task/callback that pushes camera frames to the main page's canvas
- When entering `UI_PAGE_MAIN`: stop the viewfinder, release the camera frame buffers (`app_video_stream_free_buffers()` if exposed, or similar), set the page's background to `LV_COLOR_WHITE`
- When leaving `UI_PAGE_MAIN` (entering camera/preview pages): restart viewfinder
- Measure PSRAM before/after: expect ≥7MB freed (the camera framebuffer)
- Document the PSRAM win — it's what unlocks Phase 3

### F1 — Gallery auto-plays on entry

User reports the forward/back/menu nav works correctly; only the auto-play-on-entry is missing. Task:
- Inspect `ui_extra_redirect_to_gifs_page()` → verify entry state
- If the page enters in a paused state, trigger play immediately after load
- If it already auto-plays (user thinks it might), confirm and note as no-op

### F3 — P4 photo = GIF preview placeholder

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

## Phase 3 — Parallel GIF encoding

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

## Phase 4 — Image quality: exposure sync + autofocus

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

## Phase 5 — P4 OTA via ESP32-C6 (MVP)

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
