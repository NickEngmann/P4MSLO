#!/usr/bin/env python3
"""
Test quantization quality at different bit depths.
Proves the 5-bit cube is the quality bottleneck.
"""
import os
import numpy as np
from PIL import Image
import random

SRC_DIR = os.path.dirname(os.path.abspath(__file__))
JPEGS = sorted([f for f in os.listdir(SRC_DIR) if f.endswith('.jpg')])

random.seed(42)
selected = random.sample(JPEGS, 4)
print(f"Using: {selected}")

frames = [Image.open(os.path.join(SRC_DIR, f)).convert('RGB') for f in selected]

# 1. Reference: Pillow's native quantizer (very high quality, uses octree/mediancut internally)
print("\n1. Pillow native quantize (best possible 256-color)")
ref = os.path.join(SRC_DIR, "quality_pillow_native.gif")
frames[0].save(ref, save_all=True, append_images=frames[1:], duration=300, loop=0)
print(f"   {os.path.getsize(ref)//1024}KB")

# 2. Simulate our 5-bit cube (current ESP32 code)
def quantize_Nbit(images, bits, label):
    cube = 1 << bits
    total_bins = cube ** 3
    print(f"\n{label}: {bits}-bit cube ({cube}x{cube}x{cube} = {total_bins} bins, {total_bins*4//1024}KB)")

    # Build histogram
    shift = 8 - bits
    hist = np.zeros(total_bins, dtype=np.int64)
    for img in images:
        arr = np.array(img)[::2, ::2]  # subsample 2x for speed
        r = (arr[:,:,0] >> shift).ravel()
        g = (arr[:,:,1] >> shift).ravel()
        b = (arr[:,:,2] >> shift).ravel()
        idx = (r.astype(np.int64) * cube * cube) + (g.astype(np.int64) * cube) + b.astype(np.int64)
        for i in idx:
            hist[i] += 1

    nonzero = np.count_nonzero(hist)
    print(f"   Non-zero bins: {nonzero}")

    # Popularity-based palette (fast, good enough for comparison)
    top_indices = np.argsort(-hist)[:256]
    palette = []
    for idx in top_indices:
        r = (idx // (cube * cube)) % cube
        g = (idx // cube) % cube
        b = idx % cube
        # Scale back to 8-bit
        r8 = (r << shift) | (r >> (bits - shift)) if bits > shift else r << shift
        g8 = (g << shift) | (g >> (bits - shift)) if bits > shift else g << shift
        b8 = (b << shift) | (b >> (bits - shift)) if bits > shift else b << shift
        palette.append((min(r8, 255), min(g8, 255), min(b8, 255)))
    while len(palette) < 256:
        palette.append((0, 0, 0))

    # Build LUT for fast mapping
    pal_arr = np.array(palette, dtype=np.int32)
    lut = np.zeros(total_bins, dtype=np.uint8)

    # For each possible N-bit color, find nearest palette entry
    for idx_val in range(total_bins):
        if total_bins > 500000 and idx_val % 100000 == 0:
            print(f"   Building LUT: {idx_val}/{total_bins}...")
        r = ((idx_val // (cube * cube)) % cube) << shift
        g = ((idx_val // cube) % cube) << shift
        b = (idx_val % cube) << shift
        dists = (pal_arr[:,0] - r)**2 + (pal_arr[:,1] - g)**2 + (pal_arr[:,2] - b)**2
        lut[idx_val] = np.argmin(dists)

    # Create GIF frames
    gif_frames = []
    for img in images:
        arr = np.array(img)
        h, w = arr.shape[:2]
        r = (arr[:,:,0] >> shift).astype(np.int64)
        g = (arr[:,:,1] >> shift).astype(np.int64)
        b = (arr[:,:,2] >> shift).astype(np.int64)
        pixel_idx = r * cube * cube + g * cube + b
        indices = lut[pixel_idx.ravel()].reshape(h, w)

        frame = Image.new('P', (w, h))
        flat_pal = []
        for pr, pg, pb in palette:
            flat_pal.extend([pr, pg, pb])
        frame.putpalette(flat_pal)
        frame.putdata(indices.ravel().tolist())
        gif_frames.append(frame)

    out_path = os.path.join(SRC_DIR, f"quality_{bits}bit.gif")
    gif_frames[0].save(out_path, save_all=True, append_images=gif_frames[1:],
                       duration=300, loop=0)
    print(f"   Saved: {out_path} ({os.path.getsize(out_path)//1024}KB)")

# Test different bit depths
quantize_Nbit(frames, 5, "2. Current ESP32 (5-bit)")
quantize_Nbit(frames, 6, "3. Better (6-bit)")
quantize_Nbit(frames, 7, "4. Much better (7-bit)")

print("\n=== COMPARE ===")
print("quality_pillow_native.gif  — Best possible (Pillow)")
print("quality_5bit.gif           — Current ESP32 code (grainy)")
print("quality_6bit.gif           — 6-bit cube (1MB RAM)")
print("quality_7bit.gif           — 7-bit cube (8MB RAM)")
print("\nThe 6-bit or 7-bit cube should dramatically reduce graininess.")
