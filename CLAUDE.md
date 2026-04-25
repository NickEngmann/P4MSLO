# CLAUDE.md — P4MSLO Build & Test Guide

## Quick Reference

- **Language**: C (ESP-IDF v5.5.3 framework)
- **Target**: ESP32-P4X-EYE development board
- **UI**: LVGL 8.3.11 with SquareLine Studio (4 screens, 7 fonts, 21 images)
- **Tests**: 80 tests across 8 suites, 17 mock headers
- **CI**: GitHub Actions — host tests, Docker tests, ESP-IDF cross-compilation

## Build & Run

### Host Tests (no ESP-IDF needed)
```bash
cd test && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
for t in test_*; do [ -x "$t" ] && ./"$t"; done
```

### LVGL Simulator (requires SDL2 + LVGL source)
```bash
cd test/simulator
git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git
git clone --depth 1 --branch v8.3.0 https://github.com/lvgl/lv_drivers.git
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
./p4eye_sim              # Interactive SDL2 window (720x720)
./p4eye_sim --screenshot # Headless mode — dumps PPM framebuffer screenshots
```

### ESP-IDF Build (factory_demo firmware)
```bash
# Requires ESP-IDF v5.5.3 or espressif/idf:v5.5.3 Docker image
cd factory_demo
idf.py set-target esp32p4
idf.py build
```

### Docker
```bash
# Lightweight test container
docker build -f Dockerfile.test -t p4mslo-test .
docker run --rm p4mslo-test

# Full CI image (tests + ESP-IDF cross-compile)
docker build -t p4mslo-ci .
docker run --rm p4mslo-ci
```

## Testing

### Test Suites (80 tests)

| Suite | File | Tests | What it covers |
|-------|------|-------|----------------|
| NVS Storage | `test/test_nvs_storage.c` | 10 | Settings save/load, photo count, interval state, type checking |
| GPIO/BSP | `test/test_gpio_bsp.c` | 12 | Pin state, flashlight, display, I2C, SD detect, knob init |
| UI State | `test/test_ui_state.c` | 12 | Page navigation, magnification, AI mode, SD/USB transitions |
| AI Buffers | `test/test_ai_buffers.c` | 8 | Buffer init, alignment, circular index, deinit safety |
| Sleep/Wakeup | `test/test_sleep_wakeup.c` | 5 | Wakeup cause, timer+interval, GPIO wakeup |
| UI Simulator | `test/simulator/test_ui_simulator.c` | 13 | Full UI workflow, knob debounce, menu wrap, USB interrupt |
| Phase 2 Preview | `test/test_phase2_preview.c` | 9 | Preview-scan, path format, copy-buffer size invariant, fw-version macro, WiFi config sanity |
| Phase 4 Exposure | `test/test_phase4_exposure.c` | 11 | Status-flag disjointness, SPI command uniqueness, AE header encode/decode, SET_EXPOSURE wire protocol, OV5640 gain packing, fast-mode timing |

### Mock Infrastructure (17 headers in `test/mocks/`)

**Hardware**: `nvs.h`, `nvs_flash.h`, `driver/gpio.h`, `driver/i2c_master.h`, `iot_button.h`, `iot_knob.h`
**BSP**: `bsp/esp32_p4_eye.h`, `bsp/esp-bsp.h`, `bsp/display.h`, `bsp/bsp_err_check.h`
**System**: `esp_timer.h`, `esp_sleep.h`, `esp_system.h`, `esp_memory_utils.h`, `esp_log.h`, `esp_err.h`, `esp_check.h`
**UI**: `esp_lvgl_port.h`, `ui_extra.h`
**Config**: `sdkconfig.h`, `freertos/FreeRTOS.h`, `freertos/task.h`

### LVGL Simulator

Compiles real SquareLine Studio UI code against LVGL 8.3.11 with SDL2 display backend. Hardware calls stubbed in `sim_hal.c` (includes fake colored album photos). Generates 44 PNG screenshots covering all pages in `test/simulator/screenshots/`.

- **Interactive**: SDL2 window (240x240), keyboard-to-button mapping
- **Headless**: `--screenshot` generates PNG screenshots via libpng, navigates all pages using `ui_extra_goto_page()`
- **Config**: `lv_conf.h`, `lv_drv_conf.h`, `sim_config.h`
- **Screenshot map**: See `docs/simulator-screenshots.md`

## On-Device E2E Tests (`tests/e2e/`)

Two-tier suite that talks to the P4 over `/dev/ttyACMn` via the serial
command interface. Set `P4MSLO_TEST_PORT=/dev/ttyACMn` when the P4
enumerates as something other than `/dev/ttyACM0`.

### Fast heartbeat (`run_fast.sh`, ~100 s)

For iterative development. Three tests that together smoke-test the
full P4 stack.

```bash
tests/e2e/run_fast.sh
```

| Test | What it verifies |
|------|------------------|
| `01_boot_and_liveness.py` | ping/status/cam_status respond; 0 panics, 0 watchdogs |
| `12_dma_heap_health.py`   | dma_int largest ≥ 2 KB, psram largest ≥ 8 MB, no "SPI scratch/chunk alloc failed", no "Failed to start BG worker", no video_utils OOM |
| `11_heartbeat.py`         | Page nav over all 6 menu pages, buttons, one spi_pimslo capture, gallery entry + play, sd_ls, heap health, reset_state |

### Full regression (`run_all.sh`, 10-15 min)

Pre-commit bar. Cheap smokes first, slow bg observation last. Each
test gets a 300 s hard timeout via `timeout(1)` so a pyserial hang or
firmware wedge can't hold the whole run hostage.

```bash
tests/e2e/run_all.sh
```

Order: `01 → 12 → 10 → 02 → 13 → 06 → 08 → 03 → 07 → 04 → 09 → 05`.

### Test helpers (`_lib.py`)

- `drain(s, dur, fh)` — uses `select.select()` against the fd
  directly with a hard wall-clock deadline. Bypasses pyserial's
  internal read loop that has been observed to block for 14+
  minutes when the USB CDC endpoint goes half-responsive
  (`core_sys_select` wchan). Returns within `dur + ~200 ms`.
- `reset_state(s, fh, timeout=15)` — drives `menu_goto main`, polls
  `status` until `pimslo_queue=0 pimslo_encoding=0 pimslo_capturing=0`.
  Call this at the top of every test so each one starts from a
  clean known state — this fixes the test-isolation problem where
  test 05 would pass solo (260 s) but hang after 9 prior tests left
  the gallery cache populated and bg encode mid-cycle.

## CI Pipeline (GitHub Actions)

Three parallel jobs in `.github/workflows/ci.yml`:

1. **host-tests** — Build + run 59 unit tests on `ubuntu-latest` (~30s)
2. **docker-test** — Build `Dockerfile.test`, run tests in container
3. **idf-build** — Cross-compile factory_demo in `espressif/idf:v5.5.3` container, upload firmware artifacts (~4.5min)

## Project Structure

```
P4MSLO/
├── factory_demo/              # ESP-IDF application
│   ├── main/
│   │   ├── app/              # Application logic (camera, storage, ISP, video)
│   │   ├── ui/               # SquareLine Studio generated UI
│   │   │   ├── screens/      # 4 screen definitions
│   │   │   ├── fonts/        # 7 custom fonts
│   │   │   └── images/       # 21 UI assets
│   │   └── main.c            # Entry point
│   ├── components/            # (empty — AI detection stripped out)
│   ├── sdkconfig.defaults
│   └── CMakeLists.txt
├── common_components/
│   └── esp32_p4_eye/          # Board support package
├── test/
│   ├── test_*.c               # 5 unit test files (47 tests)
│   ├── simulator/             # LVGL simulator (13 tests)
│   │   ├── sim_main.c         # SDL2 main loop + headless PNG screenshot engine
│   │   ├── sim_hal.c          # Hardware stub layer (includes fake album photos)
│   │   ├── test_ui_simulator.c
│   │   └── screenshots/       # 44 PNG screenshots covering all pages
│   ├── mocks/                 # 17 ESP-IDF mock headers
│   └── CMakeLists.txt
├── docs/                      # Documentation
│   ├── device-info.md         # ESP32-P4X-EYE hardware details, flash commands
│   └── simulator-screenshots.md # Screenshot map (44 screenshots)
├── .github/workflows/ci.yml   # CI pipeline
├── Dockerfile                 # Full CI image (tests + ESP-IDF)
└── Dockerfile.test            # Lightweight test-only image
```

## PIMSLO Stereoscopic 3D GIF Pipeline

4 ESP32-S3 cameras (OV5640 2560×1920) triggered simultaneously via ESP32-P4 GPIO34, producing oscillating 3D GIFs. Background capture + queued GIF encoding lets the user keep taking photos without waiting.

### Camera Positions (persisted in NVS)
| Device | IP | Sensor | Position |
|--------|-----|--------|----------|
| #1 | 192.168.1.119 | OV5640 5MP | 1 (leftmost) |
| #2 | 192.168.1.248 | OV5640 5MP | 2 |
| #3 | 192.168.1.66 | OV5640 5MP | 3 |
| #4 | 192.168.1.38 | OV5640 5MP | 4 (rightmost) |

Set via: `curl -X POST http://<ip>/api/v1/config/position -d '{"position":N}'`

### S3 OTA Flashing (no BOOT+RESET required)

WiFi is OFF by default on the S3s (`DISABLE_WIFI=1` is active — WiFi stack destabilizes the SPI slave — see `spi_camera.c` comments). To push new firmware without unplugging:

```bash
# 1. Bring S3 WiFi up via SPI from the P4
#    (serial command over /dev/ttyACM1 — P4 broadcasts SPI_CMD_WIFI_ON to all 4)
echo "cam_wifi_on all" > /dev/ttyACM1
# wait ~20 s for WiFi to associate on "The Garden"

# 2. Verify who came up (positions 1/2/3/4 → .119/.248/.66/.38)
for ip in 192.168.1.119 192.168.1.248 192.168.1.66 192.168.1.38; do
    curl -s --connect-timeout 2 "http://$ip/api/v1/status" | head -c 80 && echo " $ip"
done

# 3. Build + parallel OTA-flash all reachable S3s
cd esp32s3 && pio run -e xiao_esp32s3
for ip in 192.168.1.119 192.168.1.248 192.168.1.66 192.168.1.38; do
    timeout 120 curl -s -X POST "http://$ip/api/v1/ota/upload" \
        --data-binary @.pio/build/xiao_esp32s3/firmware.bin \
        -H "Content-Type: application/octet-stream" --max-time 120 &
done
wait

# 4. After reboot, disable WiFi so SPI captures are reliable
echo "cam_wifi_off all" > /dev/ttyACM1
```

**If a camera won't respond to `cam_wifi_on`**: its SPI slave task is likely stuck (see "SPI slave stuck in DATA mode" below). Try `cam_reboot N` first. If still stuck, WiFi won't come up either — USB-flash that one via BOOT+RESET.

**If SPI captures return 0/4 after a fresh OTA round**: suspect wiring on the silent cams (missing MISO series resistor, flaky CS). Do NOT run `POST /api/v1/factory-reset` to "recover" — it wipes the camera position NVS entry that maps device → pos 1-4.

**SPI capture reliability — 10/10 4-camera back-to-back at 10 MHz**: with `DISABLE_WIFI=1` on all 4 S3s and the master config in this branch, `tests/e2e/_spi_20shot.py` consistently gets 4-of-4 cameras returning JPEGs across 10+ consecutive `spi_pimslo` runs. The fixes that landed this baseline: (a) ISR-driven trigger on GPIO34 with a CS re-queue gap (`8bb11b7`), (b) 64-byte aligned `s_chunk_rx` + 64-byte aligned `s_scratch_tx/rx` for status polls (`c835c28`), (c) `chunk_rx` claimed eagerly at `spi_camera_init` while DMA-internal pool is fresh, (d) `tx_buffer = NULL` on the chunked-RX path (the slave ignores MOSI; halves DMA-internal demand from 8 KB to 4 KB). Anything that erodes any of those reverts the system to flaky / panicky territory — the diagnostic signal is `setup_dma_priv_buffer(1206): Failed to allocate priv RX buffer` followed by a `Load access fault`.

### How It Works
1. **Photo button** → takes P4 camera photo + triggers background SPI capture
2. **SPI capture task** (Core 0) → GPIO34 trigger → receives 4 JPEGs via SPI → saves to `/sdcard/p4mslo/P4M0001/pos{1-4}.jpg`
3. **GIF encode queue** (Core 1) → picks up capture folders → encodes PIMSLO GIF → saves to `/sdcard/p4mslo_gifs/P4M0001.gif` → deletes raw JPEGs

User can take photos every ~3-4 seconds. GIF encoding happens asynchronously in background.

### Manual Serial Commands
```bash
# Trigger + SPI capture + save + encode (all-in-one)
spi_pimslo 150 0.05

# Encode from existing files on SD card
pimslo 150 0.05
# (reads /sdcard/pimslo/pos{1-4}.jpg, 150ms frame delay, 0.05 parallax)

# Check pipeline status
status
# → pimslo_queue=N pimslo_encoding=0/1
```

### Parallax Algorithm
- Square center crop from source (1920×1920 from 2560×1920)
- Horizontal parallax offset per camera position within the square
- GIF sequence: 1→2→3→4→3→2→1 (6 encoded frames, 2 replayed from PSRAM cache)
- Parallax strength 0.05 works best for separate cameras (vs 0.2 for quad-lens)

### JPEG Decoding
- **tjpgd** software decoder (standalone from LVGL, renamed to `gif_jd_prepare`/`gif_jd_decomp`)
- Handles 4:2:0 AND 4:2:2 subsampled JPEGs
- Decodes directly into crop-sized output buffer (no full-image intermediate)
- ~2.2s per frame at 2560×1920
- ESP32-P4 HW JPEG decoder only works with 4:2:0 — cannot be used for OV5640/OV3660

### SPI Bus Architecture

**Bus**: SPI3_HOST on ESP32-P4, shared MISO/MOSI/CLK with per-camera CS lines.

| Signal | P4 GPIO | Direction | Notes |
|--------|---------|-----------|-------|
| CLK | 37 | P4→S3 | |
| MOSI | 38 | P4→S3 | |
| MISO | 50 | S3→P4 | 330Ω series resistor per S3 required |
| CS0 | 51 | P4→S3 #1 | |
| CS1 | 52 | P4→S3 #2 | |
| CS2 | 53 | P4→S3 #3 | |
| CS3 | 54 | P4→S3 #4 | Dynamic slot swap (SPI3 max 3 HW CS) |
| TRIGGER | 34 | P4→all S3 | Active LOW pulse, shared by all cameras |

**Speed**: 16MHz max stable. Tested: 20/26/40MHz all fail (S3 SPI slave can't keep up).

**Throughput**: ~1.3 MB/s effective per camera. A typical 600KB JPEG transfers in ~470ms.

### SPI Signal Integrity

**Required**: 330Ω series resistors on each S3 camera's MISO output line. Without these, the shared MISO bus has contention when multiple cameras drive it (only one CS is active at a time, but floating MISO outputs cause noise).

**Recommended additional measures** (for long wires or unreliable connections):
- **100pF capacitor** on CLK near each S3 — filters high-frequency ringing on clock edges. Place between CLK and GND at the S3 end.
- **Pull-up resistor (10KΩ)** on each CS line — ensures cameras stay deselected when P4 boots. The P4 firmware pre-drives CS HIGH at init, but external pull-ups add a safety margin.
- **Keep wires short** — under 15cm for the SPI bus. Longer wires increase capacitance and reduce max clock speed.
- **MOSI**: No resistor needed (single driver, P4 only).
- **CLK**: No series resistor (single driver), but a small capacitor at each S3 end helps.

**Chunk size**: Master and slave MUST use matching 4KB (4096 byte) chunk sizes. If mismatched, the slave advances by its chunk size while the master reads less, causing data gaps that appear as zeros at the end of the transfer.

### Pipeline Timing (OV5640, 4 cameras working)

Two regimes depending on which task runs the encode:

**Legacy `pimslo` serial command (on-demand task with internal stack)**

| Step | Time |
|------|------|
| GPIO34 trigger + OV5640 capture | ~600ms |
| SPI transfer × 4 cameras @ 10MHz | ~1,700ms |
| Save 4 JPEGs to SD card | ~800ms |
| **Capture + save total** | **~3,100ms** |
| | |
| Direct-JPEG .p4ms save (4 × tjpgd crop decode @ scale=0) | ~6-10s |
| GIF pass 1: palette (4 decodes) | ~7s |
| GIF pass 2: 4 forward frames @ ~12s/frame | ~50s |
| GIF pass 2: 2 replayed frames | ~18s |
| **GIF encode total** | **~85-95s** |

**photo_btn flow (`pimslo_encode_queue_task` with PSRAM stack — slower)**

The persistent `pimslo_encode_queue_task` declares a 16 KB stack, which is larger than the largest contiguous free internal block on this board (~7 KB at any point post-boot). FreeRTOS silently falls back to PSRAM, and every stack access during decode/dither/LZW takes ~100-200 ns instead of an internal-RAM cycle. Net effect: each tjpgd decode jumps from ~1.7 s → ~18 s, and Pass 2 per-frame from ~12 s → ~55 s. A real photo→encode cycle on this board is **~5-7 minutes** end to end.

Live captures still feel responsive — the user only waits ~3 s for the capture overlay to clear (SPI transfer). The slow part runs in background and the gallery shows the JPEG preview immediately as a "PROCESSING" entry until the .gif finalizes.

To recover the ~95s number you'd need to either (a) free 16 KB contiguous internal RAM (would require dropping `s_tjpgd_work`'s 32 KB BSS or `gif_encoder.c::tjwork`'s 32 KB BSS — both are required for tjpgd's hot-loop tables), or (b) restructure the encoder so the hot LZW loop runs on a separate worker thread with a smaller stack that fits internal. Open work item.

### SD Card Layout
```
/sdcard/
├── p4mslo/                    # PIMSLO capture directories
│   ├── P4M0001/               # First capture
│   │   ├── pos1.jpg           # Camera 1 JPEG
│   │   ├── pos2.jpg           # Camera 2 JPEG
│   │   ├── pos3.jpg           # Camera 3 JPEG
│   │   └── pos4.jpg           # Camera 4 JPEG
│   ├── P4M0002/               # Second capture (cleaned up after GIF encode)
│   └── ...
├── p4mslo_gifs/               # Completed GIF files
│   ├── P4M0001.gif            # GIF from capture 0001
│   ├── P4M0002.gif
│   └── ...
└── esp32_p4_pic_save/         # P4 camera photos (existing)
    ├── pic_0001.jpg
    └── ...
```

### S3 API Additions
| Method | Endpoint | Purpose |
|--------|----------|---------|
| POST | `/api/v1/config/position` | Set camera position 1-4 (persisted in NVS) |
| GET | `/api/v1/latest-photo` | Download the most recent JPEG directly |

## Known Issues

- **Gallery JPEG preview decode — slurp+scale, never per-byte-fread** — `show_jpeg()` in `app_gifs.c` reads the whole preview JPEG into a PSRAM buffer up front and hands tjpgd an in-memory pointer, and picks a tjpgd scale (0/1/2/3 = 1:1/1:2/1:4/1:8) that lands the decoded output close to the 240×240 canvas. Both matter. The original code did `fread` directly from the callback and forced scale 0 (native resolution, ~2M pixels for a 1920×1080 preview) with a naive per-src-pixel nearest-neighbor downscale; that was 14-20 s per preview on this board, which held `bsp_display_lock` inside `cmd_btn` long enough for the serial command queue to drain. Post-fix: ~650 ms at scale 2 (1920/4 = 480 px out). The hand-rolled `jpeg_out_cb` also iterates output cells (only the canvas cells this MCU contributes to) instead of every source pixel in the MCU — was O(MCU_bw·MCU_bh) per block, now O(canvas_cells_hit). For deeper timing, set `APP_GIFS_TIMING 1` at the top of `app_gifs.c` to turn on the `TIMING_MARK`/`TIMING_LOG` macros around `show_jpeg` / `slot_find_or_alloc` / `load_small_gif`.
- **Serial command USB-CDC TX saturation during captures** — `cmd_respond` goes through `printf` → USB-CDC TX. When the CDC TX buffer fills (ESP-IDF's CDC driver has a small per-endpoint queue), the serial_cmd task blocks waiting to drain, which also prevents it from reading the next queued command. During a PIMSLO capture run, `spi_camera.c::poll_and_get_size()` was emitting a `Poll: status=…` ESP_LOGI every 50 ms of polling — 200-500 lines per capture. That was enough to stall the dispatch task long enough that rapid-fire `photo_btn` commands from the host dropped (test 08 was counting 3 received vs 4 sent). Demoted to ESP_LOGD so the output only shows up with `esp_log_level_set("spi_cam", ESP_LOG_DEBUG)` at runtime.
- **LCD SPI priv TX buffer OOM → frozen LVGL** — LVGL's draw buffer lives in PSRAM (`buff_spiram=true`) and the ST7789 panel-io-spi device doesn't set `SPI_TRANS_DMA_USE_PSRAM` on its transactions, so the ESP-IDF SPI master falls into its per-flush "copy PSRAM source to a freshly-allocated DMA-capable internal scratch buffer" path. On this board the DMA-internal heap is tiny (~32 KB reserved by esp_psram + 18 KB regular RAM, of which most ends up claimed by FreeRTOS TCBs / managed-component scratch). Largest free block post-boot can drop to ~2 KB. A 4800-byte priv TX alloc then fails, `panel_st7789_draw_bitmap: io tx color failed` fires, the flush-ready callback never triggers, and taskLVGL busy-waits forever on `draw_buf->flushing` at `lv_refr.c:709`. Symptom: the screen stops refreshing, button presses still update LVGL state internally but the user sees a frozen UI. Fix in `common_components/esp32_p4_eye/esp32_p4_eye.c` uses `lvgl_port_display_cfg_t.trans_size = BSP_LCD_H_RES * 4` to make esp_lvgl_port allocate a permanent 1920-byte DMA-internal staging buffer at init. LVGL then does a PSRAM→internal memcpy through that buffer before handing the pointer to the LCD master, bypassing the per-flush alloc entirely. Debug via the `heap_caps` serial command (dumps `dma_int` free + largest-free-block).
- **Background-task stacks live in PSRAM, not internal** — `pimslo_save` (6 KB) is explicitly created with `xTaskCreatePinnedToCoreWithCaps(... MALLOC_CAP_SPIRAM)`. The other heavies (`pimslo_cap` 8 KB, `pimslo_gif` 16 KB, `gif_bg` 16 KB) call plain `xTaskCreatePinnedToCore` but FreeRTOS's allocator falls back to PSRAM whenever internal can't satisfy the request — and on this board the largest-free-internal block is ~7 KB at every point post-boot, so the fallback fires every time for the 16 KB allocations. Net: in practice all four task stacks live in PSRAM. Safe: none of these tasks run from an ISR context. Observable win: before this state, `gif_bg` was silently failing to start every boot (`Failed to start BG worker`); now it actually runs. Cost: every stack access during the encoder hot loop is a ~100-200 ns PSRAM access, which is the dominant reason Pass 2 went from ~8 s/frame (claim from the original `8f9092c` perf commit) to ~12-55 s/frame on the current board (see "Pipeline Timing" above for both regimes).
- **`encode_should_defer()` allows MAIN page** — `app_pimslo.c::encode_should_defer()` only defers when the user is on CAMERA / INTERVAL_CAM / VIDEO_MODE (viewfinder owns ~7 MB scaled_buf — actual collision). MAIN was previously in the defer list, which combined with the `gallery_ever_opened` gate meant a user who pressed photo_btn from main without ever visiting the gallery → encode never fired. Captures piled up forever as JPEG-only entries. The MAIN gate and `gallery_ever_opened` gate are both removed; the encode pipeline calls `app_video_stream_free_buffers()` before its 7 MB alloc and tolerates a foreground CAMERA-page entry mid-encode (the camera-buffer realloc is best-effort and retries on next viewfinder frame).
- **bg_worker starvation watchdog** — `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` in `sdkconfig.defaults`. The `gif_bg` background task runs pure-compute LZW frame decode + palette work on Core 1 at prio 2. A single frame decode for a large GIF is ~700 ms of uninterrupted CPU; tuning any lower than that would mean chasing yields down into ESP-IDF / LVGL code we don't own. Simpler to disable the Core 1 idle watchdog. Core 0 idle wdt stays on (`CHECK_IDLE_TASK_CPU0` defaults to y), so genuinely stuck LVGL / video-stream tasks still fire.
- **bg_worker must yield foreground** — `app_gifs_next / _prev` set `s_last_nav_ms` (gallery knob nav timestamp) and flip `s_bg_abort_current`. `bg_should_yield` returns true for 15 s after any nav, and the encode pipeline (`app_gifs_encode_pimslo_from_dir`) polls `s_bg_abort_current` at the top + between each JPEG load. `bg_encode_safe_page()` intentionally omits `UI_PAGE_GIFS` — a 50 s PIMSLO encode on the gallery page pins the JPEG decoder + 7 MB of PSRAM + SD I/O and makes gallery nav feel dead for tens of seconds. Stale captures only get re-encoded once the user leaves the gallery for ALBUM / USB / SETTINGS. Pre-rendering of `.p4ms` previews (the other bg path) still runs on any page, with a `vTaskDelay(pdMS_TO_TICKS(100))` between frames to give IDLE1 real run time.
- **SPI camera RX chunk — single 4 KB buffer, permanent, NULL tx** — `spi_camera.c` uses `spi_device_transmit` with `tx_buffer=NULL` for the receive-only JPEG pull (the S3 slave ignores MOSI during the DATA phase anyway). That halves the DMA-internal requirement from 8 KB (tx+rx) to 4 KB (rx only). The single `chunk_rx` buffer is allocated on first use via `heap_caps_malloc(..., MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)` and held forever — no per-capture heap churn, no OOM once the pool has fragmented. Before the change, "OOM for SPI chunk buffers" fired on every single capture regardless of whether the S3 cameras responded with JPEGs, and all 4 slots returned FAILED even when the slaves clearly had JPEGs ready.
- **SPI buffers MUST be 64-byte cache-aligned, not just DMA-capable** — `setup_dma_priv_buffer(1206): Failed to allocate priv RX buffer` → `Guru Meditation (Load access fault)` panic mid-capture (usually on camera 4 after 3 successful captures). Root cause: ESP-IDF's `setup_dma_priv_buffer` in `spi_master.c` triggers a per-transaction priv-buffer alloc whenever the source/dest buffer is not aligned to `cache_align_int` — 64 bytes on ESP32-P4. `heap_caps_malloc(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)` returns a 4- or 8-byte aligned block, which fails the 64-byte check; priv buf is then allocated per xfer and eventually fragments the internal pool until alloc fails → NULL deref → panic. Compounded by the PIMSLO capture task's stack living in PSRAM — all stack-allocated `uint8_t tx[8]` for poll/control transactions are unaligned AND in PSRAM, so every small transaction pays the priv-alloc cost. Fix in `spi_camera.c`: (1) permanent 16-byte `s_scratch_tx/rx` via `heap_caps_aligned_alloc(64, ...)` for status/control transactions, (2) `spi_xfer_cam` memcpys through the scratch for any ≤16-byte transaction, (3) chunk-RX uses `heap_caps_aligned_alloc(64, 4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)` claimed eagerly at `spi_camera_init` (the lazy-alloc path hits a 2.4 KB largest-free-block by first capture). The 4 KB chunk alloc at init time works because LCD's `trans_size` staging buf is already claimed by `bsp_display_start_with_config` but SD/camera transactions haven't fragmented yet.
- **ESP-IDF v5.5.3 required** — v5.5.1/5.5.2 fail component resolution
- **PlatformIO unsupported** — ESP32-P4 requires native ESP-IDF CMake
- **Simulator camera** — Viewfinder renders empty (no camera feed, expected)
- **SDL2 + libpng dependencies** — LVGL simulator needs `libsdl2-dev` and `libpng-dev`
- **LVGL/lv_drivers** — Must be manually cloned into `test/simulator/` (gitignored)
- **Chip revision — P4X v3.1 (current target)** — `sdkconfig.defaults` sets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=n` + `CONFIG_ESP32P4_REV_MIN_301=y`. Bootloader will refuse v1.x binaries on this board and vice-versa. Migration from v1.3 → v3.1 required a `rm sdkconfig && idf.py fullclean && idf.py build`. CPU runs at the v3.1 max of 400 MHz. For history: the older P4-EYE v1.3 needed `SELECTS_REV_LESS_V3=y` + `REV_MIN_100=y`.
- **`esp_timer` task stack 8192 (not 4584 default)** — the default 4584-byte stack for ESP-IDF's `esp_timer` task overflows when a log call fires from a timer callback: newlib's `_svfprintf_r` / `__ssprint_r` pushes ~1.5 KB on-stack scratch, plus context-switch overhead + caller frame = over 5 KB. Panics with `Stack protection fault` in task `esp_timer`, `MEPC` inside `__ssprint_r at vfprintf.c:268`. User reproduced this as an "album crashes on every open" rst:0xc reboot. Fix is a one-liner in `sdkconfig.defaults`: `CONFIG_ESP_TIMER_TASK_STACK_SIZE=8192`. **Critical build note**: `sdkconfig.defaults` only applies when `sdkconfig` is regenerated from scratch. After pulling this fix, run `rm factory_demo/sdkconfig && idf.py fullclean && idf.py build` before flashing, otherwise the existing sdkconfig keeps the old 4584 and the crash persists.
- **-O2 — targeted at specific hot files only** — global `CONFIG_COMPILER_OPTIMIZATION_PERF=y` deterministically hangs taskLVGL inside `lv_refr.c:709` busy-waiting on `draw_buf->flushing`. Forcing LVGL, esp_lvgl_port (`lvgl_port_lib`), esp_lcd, and esp_driver_spi back to `-Og` under global `-O2` did **not** un-stick it. Enabling `-O2` on `esp_driver_ppa/jpeg/isp/cam` or `esp_video` wdt-hangs on the first camera-page transition. Enabling `-O2` on the entire `main` component wdt-hangs during gallery navigation + concurrent encode. Current arrangement: `sdkconfig.defaults` stays at `COMPILER_OPTIMIZATION_DEBUG=y` (global `-Og`), and `factory_demo/CMakeLists.txt` opts selected pure-math source files into `-O2` via `set_source_files_properties(... PROPERTIES COMPILE_OPTIONS "-O2")`: `main/app/Gif/gif_tjpgd.c` (JPEG decode), `main/app/Gif/gif_encoder.c` (LZW + palette), `main/app/Gif/gif_decoder.c` (LZW decode), `main/app/Gif/gif_lzw.c`, `main/app/Gif/gif_quantize.c`, `main/app/Spi/spi_camera.c`. These are all pure number-crunching with no LVGL / display interaction. That drops tjpgd JPEG decode from ~2.2 s → ~1.7 s per 2560×1920 frame (~25% faster). Task stacks stay at their `-Og` defaults: widening -O2 to whole components needed `serial_cmd` → 16 KB and video-stream → 8 KB but that starved internal RAM on this build (xTaskCreate failures: "Failed to create serial command task" + "Failed to start BG worker"). Per-file -O2 doesn't inline across function boundaries enough to bloat the stack frames of tasks that never call the -O2 files.
- **Display busy-wait yielder (`wait_cb`)** — `bsp_display_start_with_config()` installs `disp->driver->wait_cb` pointing at a `vTaskDelay(1)` shim. LVGL's `refr_area_part` busy-loops on `draw_buf->flushing` whenever the previous SPI-LCD flush-ready callback hasn't fired yet. Under heavy gallery canvas updates that occasionally gets stuck for long enough that the idle watchdog on CPU 0 fires — users see this as "album crashes". The 1-tick delay lets IDLE0 run between polls so `task_wdt` stays quiet, and caps the poll rate at 1 kHz. Do **not** bump `trans_queue_depth` above 2 — every extra queue slot forces the SPI master to pre-allocate a DMA-capable TX scratch buffer from internal RAM, and the LCD+camera+SD combo on this board has barely any headroom; a 10-deep queue boots with `setup_dma_priv_buffer: Failed to allocate priv TX buffer` and the first flush fails silently.
- **Zb RISC-V extension does NOT apply** — GCC 14 lists `zba/zbb/zbc/zbs/zbkb/...` via `-march=help` but the ESP32-P4X HP core only implements `rv32imafc_zicsr_zifencei_xesppie`. Adding Zb would emit illegal instructions. Confirmed against the toolchain shipped in ESP-IDF v5.5.3.
- **Local patch: esp_lvgl_port knob uninit** — `factory_demo/patches/0001-esp_lvgl_port-knob-uninit.patch` fixes a `-Wmaybe-uninitialized` false positive that becomes `-Werror` under -O2. Applied in-tree; re-apply after any managed-component refresh.
- **ESP32-P4 HW JPEG decoder** — Cannot decode 4:2:2 subsampled JPEGs (OV5640/OV3660 output). Use tjpgd software decoder instead.
- **PSRAM fragmentation** — 32MB PSRAM but largest contiguous block is ~8.26MB after boot. Buffers over 8MB fail to allocate regardless of total free memory. After camera operations, the largest contig block drifts down to ~6.5-6.8 MB even with 7.7 MB free — close to the floor for the PIMSLO GIF decoder's 6.7 MB decode buffer. If an allocation fails, try `app_video_stream_realloc_buffers()` + `app_video_stream_free_buffers()` to consolidate the heap.
- **SPI max 16MHz** — ESP32-S3 SPI slave can't sustain 20MHz+. Tested 20/26/40MHz — all fail with timeouts.
- **Single HW JPEG decoder** — The album module and GIF encoder cannot both hold a `jpeg_decoder_engine` simultaneously. The GIF encoder releases the album's decoder before encoding and reacquires after.
- **Video-stream task stack is 4 KB** — don't put >1 KB on-stack buffers in any of its frame callbacks or in functions that callback invokes. Stack-protection fault panics in `__ssprint_r`/`_svfprintf_r` with `MCAUSE=0x1b` and `S0/FP=0` are the canonical symptom. Push big buffers to the heap or run the work on the PIMSLO capture task (8 KB stack).
- **AI detection fully removed** — the coco_detect, human_face_detect, pedestrian_detect, and esp_painter components are gone along with `main/app/AI/`. Reclaimed ~345 KB PSRAM on the photo pipeline and eliminated a heap-corruption race between `ai_detection_init_buffers` on Core 0 and the save-task's JPEG-buffer frees on Core 1. Reintroducing AI would require restoring the components, the `target_compile_options` warning suppression in `factory_demo/CMakeLists.txt`, and the `UI_PAGE_AI_DETECT` enum value.
- **Pure-display pages must free viewfinder PSRAM** — on entering GIFS / ALBUM / USB / SETTINGS from a camera page, call `app_video_stream_free_buffers()` in the redirect function. `ui_extra_leaving_main()` should NOT blindly reallocate camera buffers — only camera pages (CAMERA / INTERVAL_CAM / VIDEO_MODE) need them, and they should call `app_video_stream_realloc_buffers()` explicitly.
- **GIF playback must stop on every page transition** — `app_gifs_stop()` is called from `ui_extra_leaving_main()` and `ui_extra_redirect_to_main_page()`. A running GIF holds ~7 MB of PSRAM (decode_buffer + canvas) that will starve subsequent camera buffer allocations.
- **GIF encoder and playback can't coexist** — both want a ~7 MB scaled buffer and PSRAM only has ~8 MB contiguous. `ui_extra_redirect_to_gifs_page()` checks `app_gifs_is_encoding()` and `app_pimslo_is_encoding()` and skips auto-play when an encode is active; user can re-enter gallery once the encode finishes.
- **GIF playback memory after streaming-decoder refactor** — peak usage is now ~3.6 MB (3.5 MB `pixel_indices` + 115 KB canvas buffer + tiny frame cache entries). The old code path allocated a 6.7 MB full-res RGB565 intermediate per frame; that's gone — the decoder nearest-neighbor-scales straight from palette indices into the 240×240 canvas.
- **GIF playback framerate is decoder-limited on the first loop** — LZW decode for a 1824×1920 frame takes ~600-700 ms, and SD read of a ~1 MB compressed frame is ~500-700 ms. First loop through a GIF therefore plays at roughly 1 frame/s. Starting on loop 2, the decoder's per-frame offset map lets the caller `fseek` past already-seen frames and the canvas frame cache supplies pre-decoded pixels, so playback hits the GIF's native 150 ms framerate exactly.
- **Gallery entries are dual-path** — `app_gifs_scan()` merges `/sdcard/p4mslo_gifs/*.gif` with `/sdcard/p4mslo_previews/*.jpg` by PIMSLO stem. A capture with both files appears as a single entry carrying both paths; `play_current` flashes the JPEG via tjpgd first (~100ms) and then starts the GIF decoder — user sees something immediately instead of waiting ~700-2000ms for the first LZW frame. Capture with JPEG only (encode still running) shows the still with the center "PROCESSING" badge.
- **Gallery remembers last-viewed** — `app_gifs_scan()` preserves `current_index` by matching the previously-viewed entry's primary path against the new entries list. Leave the gallery, come back, you land on whichever GIF you were watching.
- **Persistent cross-GIF canvas cache** — `g_gif_cache[]` (up to 5 GIFs × ~700 KB canvases) lives in PSRAM across play_current calls so scrolling between recently-watched GIFs replays them instantly instead of re-decoding. LRU-evicted when full. Flushed on gallery exit (`app_gifs_flush_cache()` called from `ui_extra_leaving_main` / `redirect_to_main_page`) so camera / video paths get the PSRAM back.
- **Never draw directly onto the canvas pixel buffer from app_gifs timers** — `lv_canvas_draw_rect` + `lv_canvas_draw_text` on `s_ctx.canvas_buffer` inside the video-stream display lock caused a black-screen lock-up (the canvas write conflicted with something LVGL was doing concurrently). Use a separate `lv_label` widget positioned over the canvas instead — it lives on its own LVGL object layer and costs zero per-frame work.
- **Main menu layout** — CAMERA, ALBUM, INTERVAL CAM, VIDEO MODE, USB DISK, SETTINGS (in that order). "ALBUM" routes to UI_PAGE_GIFS — it IS the PIMSLO gallery (GIFs + p4ms previews + delete modal). The legacy P4-photo album screen (UI_PAGE_ALBUM) was removed from the menu but the enum + code paths remain compiled so encoder/decoder plumbing still works.
- **Gallery delete modal (mirrors album behavior)** — trigger/encoder button (BSP_BUTTON_ED, same button that takes photos in the camera app) opens the delete modal when the gallery is idle; both encoder AND menu button (BSP_BUTTON_1) act as YES/NO selectors when the modal is open. Menu button on a closed modal exits to main. Up/down toggles YES ↔ NO. Selection state is tracked via an explicit `delete_yes_selected` boolean — LVGL's `LV_STATE_FOCUSED` machinery was replaced because its automatic re-focus behavior was flipping the initial highlight unexpectedly on modal open. `app_gifs_delete_current()` unlinks the `.gif`, `.p4ms`, AND preview `.jpg` for the current entry, evicts the cached canvas slot, and triggers an `lv_async_call`-marshalled rescan.
- **P4 camera sensor init failures are non-fatal** — `app_video_stream_init` is no longer wrapped in `ESP_ERROR_CHECK`. If the OV2710's SCCB probe fails (ribbon loose, rail dip), main.c logs a loud error and continues booting. The rest of the UI works; only the live viewfinder is dark. Previously this path crashed + reboot-looped, flashing the display every ~1 s.
- **Sparse PIMSLO camera positions** — when SPI captures succeed non-consecutively (e.g. cams 1, 3, 4 succeed but 2 fails), the encoder loads into a compact `jpeg_data[0..num_cams-1]` array but tracks each JPEG's true camera position in `src_pos[]`. Parallax crops and filesystem reads use `src_pos[i]` so the resulting GIF still reflects the correct perspective spread.
- **Sub-2-cam captures are dropped at the capture task** — if fewer than 2 cameras return usable JPEGs, `pimslo_capture_task` deletes the preview + partial capture dir and skips enqueueing. Prevents the "PROCESSING forever" orphan entry users saw when a failed capture would show up in the gallery with no way to encode.
- **PIMSLO capture → save → GIF encode runs on 3 tasks** — capture task (Core 0) owns the SPI transfer only; it enqueues raw JPEG buffers to `s_save_queue` and immediately clears `s_capturing` + reallocates viewfinder buffers. `pimslo_save_task` (Core 1, 6 KB stack, prio 4) drains the queue: fwrite 4 × pos{1-4}.jpg, save preview, enqueue GIF job. `pimslo_encode_queue_task` (Core 1, 16 KB, prio 4) runs the actual GIF encode. The `s_saving` flag keeps `app_pimslo_is_capturing()` truthy across the capture→save handoff so the "saving..." overlay stays visible. Shrinks the user-visible overlay duration from ~15-20 s (SPI + inline SD writes) to ~3 s (SPI only) — they can fire the next photo while the save + encode continue in the background. Reinstated after the AI-detection removal eliminated the heap-corruption race that forced inline saves.
- **JPEG integrity check before save** — `pimslo_capture_task` verifies SOI (`FF D8`) and EOI (`FF D9`) markers on each SPI-returned JPEG before writing to SD. Corrupted transfers (truncated / mid-stream bit flip) get dropped with a `corrupted JPEG (SOI=0 EOI=1)` log.
- **Gallery "QUEUED" vs "PROCESSING" badge** — JPEG-only entries show "PROCESSING" when `app_pimslo_encoding_capture_num()` matches their stem, "QUEUED" otherwise. Text is set each time `app_gifs_play_current()` runs.
- **Empty-album / SD-error overlay** — `s_ctx.empty_label` is a centered lv_label shown when `app_gifs_get_count() == 0`. Text swaps between "Album empty / Take a photo from the camera" and "SD card not detected / Insert a card and reboot" based on `ui_extra_get_sd_card_mounted()`. Refreshed via `app_gifs_refresh_empty_overlay()`.
- **Gallery scan preserves view by STEM, not exact path** — `app_gifs_scan()` restores `current_index` by matching either the exact previous primary path OR the PIMSLO stem (P4Mxxxx). Without the stem fallback, an entry whose JPEG-only state got promoted to GIF during encode would see its primary path flip from `.jpg` to `.gif` and the scan would bounce the user to index 0 instead of following them onto the same capture.
- **bg encoder failure blacklist** — `bg_find_jpeg_only` + `bg_find_unprocessed_gif` both skip blacklisted paths. Session-scoped in-memory list, cleared on reboot. Prevents the bg worker from thrashing on a broken `.gif` or a capture dir with <2 pos files forever.
- **LVGL task stack is 8 KB** — set in `common_components/esp32_p4_eye/esp32_p4_eye.c`. `lv_async_call` from bg tasks schedules `app_gifs_scan()` + `app_gifs_play_current()` which together use more than the ESP-IDF default 4 KB (overflowed under -Og with tjpgd's 32 KB work buffer allocated on stack in one path — that's now static, but keeping 8 KB gives headroom).

## Open Research Questions

(None outstanding. Historical list moved to the Known Issues section as things get fixed — see "Gallery JPEG preview decode" entry there for the biggest one.)

## Flash via Docker

```bash
docker run --rm -v $(pwd):/workspace -w /workspace/factory_demo \
  --device=/dev/ttyACM0 espressif/idf:v5.5.3 \
  bash -c ". \$IDF_PATH/export.sh && idf.py -p /dev/ttyACM0 flash"
```

## ABSOLUTE REQUIREMENTS

- **NEVER downscale resolution.** All GIFs must be encoded at the full source resolution (square crop from OV5640 2560×1920 → 1920×1920, or full 1920×1080 from P4 camera). Image quality is the #1 priority. If memory is tight, find another way (free buffers, process one frame at a time, use smaller intermediate structures) — but NEVER reduce the output resolution.
- **Camera JPEG quality minimum 4.** Never set `CAMERA_JPEG_QUALITY` below 4. Quality 2 produces non-standard Huffman tables that software decoders (tjpgd, esp_new_jpeg) cannot handle.
- **SPI chunk size must match.** P4 master and S3 slave MUST use the same chunk size (4096 bytes). Mismatch causes data corruption (slave advances by its chunk size, skipping data).

## Gotchas

1. The Dockerfile uses `espressif/idf:v5.5.1` but CI uses `v5.5.3` — use v5.5.3
2. LVGL simulator executable is `p4eye_sim`, not `p4mslo_sim`
3. NVS mock has 64-entry limit — keep test datasets small
4. GPIO mock tracks 64 pins — verify pin numbers match hardware
5. Sleep/wakeup tests need manual timer setup in mocks
6. AI buffer alignment is critical — use `esp_memory_utils` mock
7. Chip rev v1.3 needs `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_100=y` in sdkconfig.defaults
8. `ui_extra_clear_page()` must hide `ui_PanelCameraSettings` — otherwise ISP sliders bleed into other pages
9. The popup timer (`lv_popup_timer`) is shared across pages — call `ui_extra_cancel_popup_timer()` when programmatically switching pages
10. OV5640 cameras produce 4:2:2 JPEGs at QSXGA — the P4 HW JPEG decoder rejects these with `ESP_ERR_INVALID_STATE (0x103)`. Use tjpgd software decoder.
11. SPI clock speeds above 16MHz cause all S3 cameras to timeout — the SPI slave firmware can't keep up. Don't try 20MHz+.
12. After failed high-speed SPI tests, S3 cameras may get stuck in DATA mode. OTA reflash to reset their SPI state.
13. System time defaults to 2026-01-01 (set in `main.c` via `settimeofday`). Without RTC, files would otherwise get 1980 timestamps.
14. Background tasks that run work from a frame callback must check their own stack size. The video-stream task's 4 KB stack blew up the first time we added a 2 KB file-copy buffer in the photo-save path — move such work to the PIMSLO capture task (8 KB) instead.
15. The P4-to-S3 SPI protocol now supports commands in the `0x01–0x0F` range. The 10× burst retry in `spi_camera_send_control` makes the whole range wire-reliable, NOT just `0x01–0x07` as earlier comments claimed. Adding new commands (e.g. SPI_CMD_AUTOFOCUS=0x08, SPI_CMD_SET_EXPOSURE=0x09) works — slave's scan loop matches any known command byte in rx[0..7].
16. The extended SPI IDLE header carries current OV5640 AE gain + exposure in bytes 5-9 so the master can snapshot one camera's exposure state via a normal status poll and broadcast it to the others via SPI_CMD_SET_EXPOSURE. See `SPI_IDLE_HEADER_AE_GAIN_OFFSET` and `SPI_IDLE_HEADER_AE_EXPOSURE_OFFSET` in `esp32s3/src/spi/SPISlave.h` + mirror constants on P4 side. Any master still reading only 5 bytes continues to work unchanged.
17. OV5640 autofocus requires a proprietary firmware blob (~4 KB) loaded to sensor register 0x8000 via SCCB — esp32-camera v2.0.4 does NOT bundle this. `CameraManager::autofocus()` is currently a stub that returns true immediately so the master's AF_LOCKED polling doesn't hang. Real AF integration path is documented in `docs/phase-plan.md` Phase 4.
18. P4 WiFi goes through the onboard ESP32-C6 via `espressif/esp_wifi_remote` + `esp_hosted` managed components. On boot WiFi is OFF; user brings it up with the `wifi_start` serial command. C6 SDIO transport pins on the P4-EYE may not match the esp_hosted defaults — needs schematic confirmation. See `factory_demo/main/app/Net/app_p4_net.c`.
