# GIF Encoder Optimization Log

## Hardware
- **CPU**: ESP32-P4, dual-core RISC-V @ 360MHz
- **ISA**: `rv32imafc_zicsr_zifencei_xesploop_xespv`
- **RAM**: 32MB PSRAM @ 200MHz, ~350KB internal SRAM
- **Target**: 4-frame 1920×1080 animated GIF with Floyd-Steinberg dithering

## Results Summary

| Optimization | dither+LZW/frame | Total (4 frames) | Speedup vs baseline |
|---|---|---|---|
| Baseline (no LUT, -Og) | ~40,000ms | ~160s | 1x |
| **+ RGB565 LUT (64KB)** | ~4,500ms | ~30s | **5.3x** |
| + XOR hash + generation counter | ~4,300ms | ~28s | 5.7x |
| + Internal RAM LUT (64KB SRAM) | ~4,200ms | ~27s | 5.9x |
| + Internal RAM error buffers (23KB) | ~4,000ms | ~26s | 6.2x |
| + Row pixel prefetch (4KB SRAM) | ~3,950ms | ~26s | 6.2x |
| + 32KB file write buffer | ~3,950ms | ~26s | 6.2x |
| **+ -O3 compiler optimization** | ~3,600ms | ~24s | **6.7x** |
| + SIMD memzero for error buffers | ~3,600ms | ~24s | 6.7x |

**Final: ~24 seconds for a 4-frame 1920×1080 dithered GIF (down from 160s)**

## What Worked

### 1. RGB565 → Palette LUT (biggest win: 9x per-pixel speedup)
Precompute a 64KB lookup table mapping every possible RGB565 value to its nearest palette index. Built once after palette finalization (~1.3s). Eliminates the 256-entry linear search per pixel — each pixel is now a single array lookup.

### 2. -O3 Compiler Optimization (~15% improvement)
Setting `-O3 -funroll-loops -ffast-math` for the GIF encoder/LZW/quantizer files while the rest of the project stays at `-Og`. The compiler does better register allocation, loop unrolling, and instruction scheduling.

### 3. Internal RAM Placement (~10% improvement)
Moving hot data structures from PSRAM (200MHz, high latency) to internal SRAM (~0 latency):
- LUT: 64KB in SRAM (accessed every pixel)
- Error diffusion buffers: 23KB in SRAM (read+written every pixel)
- Row pixel cache: 4KB in SRAM (prefetch from PSRAM once per row)
- Palette RGB arrays: 768 bytes on stack

### 4. LZW Hash Table Optimization
- XOR-shift hash instead of multiply (faster on RISC-V)
- Generation counter eliminates memset on dictionary reset (~500 resets/frame)
- 16K→8K entries for better cache behavior

### 5. Two-Pass Row Processing
Separate dithering (Pass A: pixel conversion + error distribution → indices array) from LZW encoding (Pass B: feed indices to compressor). Better cache behavior since each pass accesses memory sequentially.

## What Didn't Help Much

### Separated Error Distribution
Collecting all errors in a buffer then distributing in three linear loops (below-left, below-center, below-right). Added overhead from the extra buffer and three passes outweighed the simpler loops. ~4,000ms vs 3,600ms — worse.

### int8_t Error Buffers
Using int8_t instead of int16_t for error diffusion. PSRAM accesses are word-aligned regardless, so half-sized buffers don't reduce bandwidth. Clamping to ±127 also loses precision.

### Direct-Indexed LZW Dictionary (4MB table)
Replacing the hash table with a direct `dict[prefix*256+suffix]` array. Zero collisions but 4MB allocation competed with other PSRAM buffers, causing allocation failures. The hash table with generation counter is a better tradeoff.

## What We Investigated but Didn't Implement

### xespv SIMD Vector Instructions
The ESP32-P4 has 128-bit vector registers (q0-q7) with operations like `esp.vadd.s16` (8 × int16 parallel add). We wrote a `gif_simd.S` with `esp.zero.q` for fast memzero and `gif_simd_distribute_below` for vectorized error distribution.

**Challenge**: Floyd-Steinberg dithering has horizontal dependencies (pixel x+1 depends on pixel x's error), making the core loop inherently serial. SIMD can only help the vertical error distribution (below-row), which is a small fraction of total time.

**Opportunity**: The `esp.vmulas.s16.qacc` instruction could accelerate the error × fraction multiplies (7/16, 5/16, 3/16, 1/16) on 8 values at once, but requires careful assembly to handle the accumulator register protocol.

### Parallel Decode/Encode Pipeline
Decode frame N+1 on a worker task while encoding frame N. Would save ~500ms/frame (the JPEG decode time). Requires double-buffering the scaled pixel buffer (2 × 4MB) and FreeRTOS synchronization. Estimated improvement: ~2 seconds total.

### DMA2D Block Transfers
The ESP32-P4 has a 2D DMA engine that could accelerate rectangle-to-rectangle memory copies. Could replace the row prefetch `memcpy` with async DMA, overlapping data transfer with computation.

## Performance Breakdown (Current Best)

For a single 1920×1080 frame at -O3:
- **JPEG decode + copy**: ~500ms (JPEG HW decoder + memcpy from decode buffer)
- **Dithering loop**: ~2,000ms (2M pixels × RGB565→RGB888 + error + clamp + LUT + error distribution)
- **LZW encoding**: ~1,500ms (2M pixels × hash lookup + bit packing + SD card writes)
- **Total**: ~4,000ms/frame

The ~2,000ms dithering is **~360 cycles/pixel** at 360MHz — close to the minimum for the arithmetic operations involved. The ~1,500ms LZW is **~270 cycles/pixel** for hash table lookup + variable-length code packing.

## Future Ideas

1. **Ordered (Bayer) dithering** instead of Floyd-Steinberg — fully parallelizable (no pixel dependencies), SIMD-friendly, but lower visual quality
2. **ESP32-P4 second core** — run dithering on core 0 and LZW on core 1 with a ring buffer between them
3. **Reduced-precision dithering** — use 4-bit error fractions (shift only, no multiply) for the Floyd-Steinberg weights
4. **Frame delta encoding** — GIF supports only encoding changed pixels between frames, dramatically reducing LZW work for similar consecutive frames
5. **PSRAM burst optimization** — align data structures to cache lines and access in sequential bursts to maximize PSRAM throughput
