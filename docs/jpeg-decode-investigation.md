# JPEG Decode Investigation — OV3660 4:2:2 vs ESP32-P4

## Summary

The ESP32-P4 hardware JPEG decoder cannot decode JPEGs from the OV3660 cameras. After extensive debugging, two root causes were identified:

1. **4:2:2 subsampling incompatibility**: OV3660 produces YUV 4:2:2 JPEGs (SOF0 Y component: H=2, V=1). The ESP32-P4 HW JPEG decoder only works with 4:2:0 (H=2, V=2). The P4's own camera produces 4:2:0.

2. **SPI transfer data corruption**: Even with software decoders (esp_new_jpeg, tjpgd) that theoretically handle 4:2:2, decoding fails with "corrupted data" errors. The JPEG headers (FF D8) and endings (FF D9) are intact, but entropy data in the middle has bit errors from the SPI transfer.

## Evidence

### 4:2:2 vs 4:2:0 Subsampling

P4 internal camera JPEG SOF0 (works):
```
FF C0 00 11 08 04 38 07 80 03 01 22 00 02 11 01 03 11 01
                                   ^^ Y: H=2, V=2 = 4:2:0
```

OV3660 camera JPEG SOF0 (fails):
```
FF C0 00 11 08 06 00 08 00 03 01 21 00 02 11 01 03 11 01
                                   ^^ Y: H=2, V=1 = 4:2:2
```

### ESP32-P4 HW Decoder Error
```
jpeg.decoder: the number of data units obtained after decoding a frame of image
is different from the number of data units calculated based on the image
resolution configured by the software
```
This is returned as `ESP_ERR_INVALID_STATE (0x103)` for all buffer sizes and allocation methods.

### Software Decoder Verification
- **PIL/Pillow on host**: Opens and verifies OV3660 JPEGs perfectly (4:2:2 supported)
- **esp_new_jpeg on P4**: Fails with errors -1 (FAIL), -3 (NO_MORE_DATA), -5 (BAD_DATA) at various points in entropy data
- **tjpgd on P4**: Fails with JDR_FMT1 (6) = "expected RSTn marker not detected (corrupted data)"
- **tjpgd on P4 with P4 camera JPEGs**: Works perfectly — decoded 6 frames at 1920x1080

### SPI Transfer Corruption
The JPEG files transferred via SPI at 16MHz with 4 cameras sharing the bus (330Ω MISO series resistors) have subtle bit errors in the entropy data section. Headers and EOI markers are intact. The corruption is consistent per camera position, suggesting it's related to bus timing/signal integrity rather than random noise.

## What Was Tried

| Approach | Result |
|----------|--------|
| HW decoder with 2x buffer (6.3MB) | 0x103 — data units mismatch |
| HW decoder with 2.5x buffer (7.9MB) | 0x103 — same error |
| HW decoder with jpeg_alloc_decoder_mem | Allocated OK, still 0x103 |
| Album JPEG decoder released first | Confirmed single HW decoder, still 0x103 |
| Fresh decoder engine per frame | Still 0x103 |
| Camera JPEG quality 4→10 | Smaller files, still 0x103 (4:2:2 is sensor-level) |
| JPEG marker stripping (APP0) | Removed 18 bytes, no effect on decode |
| esp_new_jpeg SW decoder (RGB565) | Errors -1/-3/-5 on different cameras |
| esp_new_jpeg block mode | Partial decode (13-179 of 192 blocks), then errors |
| esp_new_jpeg RGB888 output | 9.4MB allocation fails (PSRAM fragmentation, largest block 8.26MB) |
| Capture raw YUV422 on S3 | QXGA resolution doesn't support raw mode |
| S3 JPEG re-encoding (YUV422→JPEG 4:2:0) | esp_new_jpeg decoder fails on S3 too |
| tjpgd (tiny JPEG decoder) | Works with 4:2:0 P4 JPEGs, fails on SPI-transferred OV3660 JPEGs |

## PSRAM Fragmentation

With all camera/album buffers freed, PSRAM shows:
- **Total free**: ~21 MB
- **Largest contiguous block**: 8.26 MB (consistent)

This means any allocation > 8.26MB fails regardless of total free. The fragmentation is caused by boot-time allocations (LVGL buffers, AI models, etc.) that split the 32MB PSRAM into non-contiguous regions.

## Fixes Required

### For 4:2:2 Subsampling
The OV3660 at QXGA (2048x1536) always outputs YUV 4:2:2 — this is a sensor hardware limitation. Options:
1. **Re-encode on S3**: Capture JPEG, decode to RGB888, re-encode as 4:2:0. Requires esp_new_jpeg to work with these JPEGs (currently fails).
2. **Use tjpgd on P4**: Already integrated and working. Decodes 4:2:2 correctly — the issue is only with corrupted SPI data.
3. **Change camera resolution**: Lower resolutions might use 4:2:0, but the user requires full resolution.

### For SPI Corruption
1. **Reduce SPI clock**: Try 10MHz or 8MHz for better signal integrity
2. **Add CRC verification**: Checksum each 4KB chunk, retry on mismatch
3. **Reduce bus load**: Use separate SPI buses for camera pairs
4. **Increase series resistance**: 470Ω or 1KΩ instead of 330Ω

## Changes Made in This Branch

### P4 Firmware
- **tjpgd software JPEG decoder**: Standalone copy from LVGL, renamed to avoid symbol conflicts (`gif_jd_prepare`, `gif_jd_decomp`). Works with 32KB work buffer. Decodes to RGB565 via MCU-block callback.
- **Album JPEG decoder release**: `app_album_release_jpeg_decoder()` / `app_album_reacquire_jpeg_decoder()` — releases the album's HW JPEG decoder handle during GIF encoding (ESP32-P4 only has one HW decoder).
- **PIMSLO fast pipeline**: `app_gifs_create_pimslo_fast()` — in-memory SPI→SD→encode path.
- **sd_cp serial command**: Copy files on SD card for testing.
- **JPEG marker stripping**: Strips APP0 and other non-essential markers (currently disabled since using SW decoder).

### S3 Firmware
- **Camera quality**: Changed from 2 to 4 (reverted from 10)
- **JpegReencoder module**: Created but not active (re-encoding fails due to esp_new_jpeg decoder issues with OV3660 JPEGs)
- **idf_component.yml**: Added esp_new_jpeg dependency for S3

### Performance Data (Working P4 Camera JPEGs)
| Stage | Time |
|-------|------|
| tjpgd JPEG decode (1920x1080) | ~1.1s |
| Floyd-Steinberg dithering + LZW | ~3.5s |
| **Per frame total** | **~4.6s** |
| **6 frames total** | **~28s** |
| **Estimated 7 frames (with reuse optimization)** | **~23s** |

### Frame Reuse Optimization (Not Yet Implemented)
The oscillation sequence 1→2→3→4→3→2→1 has duplicate frames (3,2,1 appear twice). Caching the LZW-encoded frame data during pass 2 forward, then replaying for reverse, would save 3 frame encodes (~13.5s). This is a purely software optimization independent of the JPEG decode issue.
