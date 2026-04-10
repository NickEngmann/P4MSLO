#!/usr/bin/env python3
"""
GIF encoder test harness — fast version using Pillow + numpy.

Tests the color pipeline to find the BGR565 extraction bug.
"""

import os
import numpy as np
from PIL import Image
import random

SRC_DIR = os.path.dirname(os.path.abspath(__file__))
JPEGS = sorted([os.path.join(SRC_DIR, f) for f in os.listdir(SRC_DIR) if f.endswith('.jpg')])

print(f"Found {len(JPEGS)} source JPEGs")

# Pick 4 random images
random.seed(42)
selected = random.sample(JPEGS, min(4, len(JPEGS)))
print(f"Selected: {[os.path.basename(f) for f in selected]}")

# ============================================================
# 1. REFERENCE GIF using Pillow (known-good baseline)
# ============================================================
print("\n=== 1. Reference GIF (Pillow, correct colors) ===")
frames = [Image.open(p).convert('RGB') for p in selected]
ref_path = os.path.join(SRC_DIR, "reference_pillow.gif")
frames[0].save(ref_path, save_all=True, append_images=frames[1:],
               duration=300, loop=0, optimize=False)
print(f"  Saved: {ref_path} ({os.path.getsize(ref_path)//1024}KB)")

# ============================================================
# 2. Test BGR565 roundtrip quality (simulate ESP32 pipeline)
# ============================================================
print("\n=== 2. BGR565 roundtrip test (simulating JPEG decoder output) ===")

def test_roundtrip(img_path, label):
    """Convert RGB → BGR565 → back to RGB, save as GIF to check colors."""
    img = Image.open(img_path).convert('RGB')
    arr = np.array(img, dtype=np.uint16)
    r, g, b = arr[:,:,0], arr[:,:,1], arr[:,:,2]

    # Encode to BGR565: B in [15:11], G in [10:5], R in [4:0]
    bgr565 = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)

    # Decode BGR565 → RGB888 (our C code's extraction)
    b_out = ((bgr565 >> 11) & 0x1F).astype(np.uint8)
    g_out = ((bgr565 >> 5) & 0x3F).astype(np.uint8)
    r_out = (bgr565 & 0x1F).astype(np.uint8)

    r8 = (r_out << 3) | (r_out >> 2)
    g8 = (g_out << 2) | (g_out >> 4)
    b8 = (b_out << 3) | (b_out >> 2)

    result = np.stack([r8, g8, b8], axis=2).astype(np.uint8)
    return Image.fromarray(result)

# Test BGR565 roundtrip on first image
rt_img = test_roundtrip(selected[0], "bgr565")
rt_path = os.path.join(SRC_DIR, "roundtrip_bgr565.png")
rt_img.save(rt_path)
print(f"  BGR565 roundtrip: {rt_path}")

# ============================================================
# 3. Simulate full encoder pipeline with Pillow quantization
# ============================================================
print("\n=== 3. BGR565 pipeline + Pillow quantize (isolate color vs quantizer) ===")

bgr_frames = []
for path in selected:
    img = Image.open(path).convert('RGB')
    arr = np.array(img, dtype=np.uint16)
    r, g, b = arr[:,:,0], arr[:,:,1], arr[:,:,2]

    # RGB → BGR565 → RGB888 roundtrip
    bgr565 = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)
    b_out = ((bgr565 >> 11) & 0x1F).astype(np.uint8)
    g_out = ((bgr565 >> 5) & 0x3F).astype(np.uint8)
    r_out = (bgr565 & 0x1F).astype(np.uint8)
    r8 = (r_out << 3) | (r_out >> 2)
    g8 = (g_out << 2) | (g_out >> 4)
    b8 = (b_out << 3) | (b_out >> 2)

    result = np.stack([r8, g8, b8], axis=2).astype(np.uint8)
    bgr_frames.append(Image.fromarray(result))

bgr_path = os.path.join(SRC_DIR, "bgr565_pillow_quantize.gif")
bgr_frames[0].save(bgr_path, save_all=True, append_images=bgr_frames[1:],
                    duration=300, loop=0)
print(f"  Saved: {bgr_path} ({os.path.getsize(bgr_path)//1024}KB)")

# ============================================================
# 4. Test with WRONG channel order (R/B swapped)
#    This simulates what happens if we extract as RGB565 when data is BGR565
# ============================================================
print("\n=== 4. WRONG channel extraction (R/B swapped) ===")

wrong_frames = []
for path in selected:
    img = Image.open(path).convert('RGB')
    arr = np.array(img, dtype=np.uint16)
    r, g, b = arr[:,:,0], arr[:,:,1], arr[:,:,2]

    # Encode as BGR565
    bgr565 = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)

    # WRONG: extract as if it's RGB565 (R in high bits, B in low)
    r_wrong = ((bgr565 >> 11) & 0x1F).astype(np.uint8)  # Actually B!
    g_mid = ((bgr565 >> 5) & 0x3F).astype(np.uint8)
    b_wrong = (bgr565 & 0x1F).astype(np.uint8)            # Actually R!

    r8 = (r_wrong << 3) | (r_wrong >> 2)
    g8 = (g_mid << 2) | (g_mid >> 4)
    b8 = (b_wrong << 3) | (b_wrong >> 2)

    result = np.stack([r8, g8, b8], axis=2).astype(np.uint8)
    wrong_frames.append(Image.fromarray(result))

wrong_path = os.path.join(SRC_DIR, "wrong_channel_order.gif")
wrong_frames[0].save(wrong_path, save_all=True, append_images=wrong_frames[1:],
                     duration=300, loop=0)
print(f"  Saved: {wrong_path} ({os.path.getsize(wrong_path)//1024}KB)")

# ============================================================
# 5. Simulate the 5-bit quantizer (fast numpy version)
# ============================================================
print("\n=== 5. 5-bit median-cut quantizer simulation ===")

def quantize_5bit(images_rgb, n_colors=256):
    """Simulate the C encoder's 5-bit median-cut quantizer using numpy."""
    # Subsample all images to build color histogram
    all_r5, all_g5, all_b5 = [], [], []
    for img in images_rgb:
        arr = np.array(img)
        # Subsample every 4th pixel
        flat = arr.reshape(-1, 3)[::4]
        all_r5.append(flat[:, 0] >> 3)  # 8-bit → 5-bit
        all_g5.append(flat[:, 1] >> 3)
        all_b5.append(flat[:, 2] >> 3)

    r5 = np.concatenate(all_r5)
    g5 = np.concatenate(all_g5)
    b5 = np.concatenate(all_b5)

    # Build 5-bit histogram
    idx = (r5.astype(np.int32) << 10) | (g5.astype(np.int32) << 5) | b5.astype(np.int32)
    hist = np.bincount(idx, minlength=32*32*32)

    # Extract unique colors with their counts
    nonzero = np.nonzero(hist)[0]
    colors = []
    for i in nonzero:
        r = (i >> 10) & 0x1F
        g = (i >> 5) & 0x1F
        b = i & 0x1F
        colors.append((r, g, b, int(hist[i])))

    print(f"  Unique 5-bit colors: {len(colors)}")

    # Simple popularity-based palette (top N most popular colors)
    # This is simpler than median-cut but shows the 5-bit quantization effect
    colors.sort(key=lambda x: -x[3])
    palette = []
    for r5, g5, b5, cnt in colors[:n_colors]:
        r8 = (r5 << 3) | (r5 >> 2)
        g8 = (g5 << 3) | (g5 >> 2)
        b8 = (b5 << 3) | (b5 >> 2)
        palette.append((r8, g8, b8))
    while len(palette) < 256:
        palette.append((0, 0, 0))

    return palette

# Quantize using our 5-bit histogram
palette_5bit = quantize_5bit(frames)
print(f"  First 10 palette entries: {palette_5bit[:10]}")

# Create frames using this palette
quant_frames = []
for img in frames:
    arr = np.array(img)
    h, w = arr.shape[:2]
    r, g, b = arr[:,:,0], arr[:,:,1], arr[:,:,2]

    # Find nearest palette entry for each pixel (vectorized)
    pal_arr = np.array(palette_5bit[:256], dtype=np.int32)

    # Downsample for speed: use 5-bit matching
    r5 = (r >> 3).astype(np.int32)
    g5 = (g >> 3).astype(np.int32)
    b5 = (b >> 3).astype(np.int32)

    # Build a lookup from 5-bit color to nearest palette index
    # (This is much faster than per-pixel nearest-neighbor)
    pal_r5 = pal_arr[:, 0] >> 3
    pal_g5 = pal_arr[:, 1] >> 3
    pal_b5 = pal_arr[:, 2] >> 3

    # For each pixel, find nearest 5-bit palette entry
    pixel_idx = (r5 << 10) | (g5 << 5) | b5
    # Build reverse lookup
    lut = np.zeros(32*32*32, dtype=np.uint8)
    for i in range(min(len(palette_5bit), 256)):
        pr5, pg5, pb5 = palette_5bit[i][0] >> 3, palette_5bit[i][1] >> 3, palette_5bit[i][2] >> 3
        pidx = (pr5 << 10) | (pg5 << 5) | pb5
        if lut[pidx] == 0 or i < lut[pidx]:
            lut[pidx] = i

    # Map all unknown entries to nearest palette color
    for idx_val in range(32*32*32):
        if lut[idx_val] == 0:
            ir = (idx_val >> 10) & 0x1F
            ig = (idx_val >> 5) & 0x1F
            ib = idx_val & 0x1F
            best = 0
            best_d = 999999
            for pi in range(min(len(palette_5bit), 256)):
                pr, pg, pb = palette_5bit[pi]
                d = ((ir*8 - pr)**2 + (ig*8 - pg)**2 + (ib*8 - pb)**2)
                if d < best_d:
                    best_d = d
                    best = pi
            lut[idx_val] = best

    indices = lut[pixel_idx.ravel()].reshape(h, w)

    frame_img = Image.new('P', (w, h))
    flat_pal = []
    for r8, g8, b8 in palette_5bit:
        flat_pal.extend([r8, g8, b8])
    frame_img.putpalette(flat_pal)
    frame_img.putdata(indices.ravel().tolist())
    quant_frames.append(frame_img)

q5_path = os.path.join(SRC_DIR, "quantized_5bit.gif")
quant_frames[0].save(q5_path, save_all=True, append_images=quant_frames[1:],
                     duration=300, loop=0)
print(f"  Saved: {q5_path} ({os.path.getsize(q5_path)//1024}KB)")

print("\n=== COMPARISON ===")
print("Open these files side by side:")
print(f"  1. reference_pillow.gif        — CORRECT (Pillow native)")
print(f"  2. bgr565_pillow_quantize.gif  — BGR565 roundtrip + Pillow quantize")
print(f"  3. wrong_channel_order.gif     — R/B swapped (what BAD looks like)")
print(f"  4. quantized_5bit.gif          — Our 5-bit quantizer quality")
print(f"  5. animation_43616.gif         — ACTUAL device output")
print(f"\nIf #2 looks correct but #5 looks like #3, the ESP32 decoder outputs RGB565 not BGR565.")
print(f"If #4 looks bad, the 5-bit quantizer is the quality bottleneck.")
