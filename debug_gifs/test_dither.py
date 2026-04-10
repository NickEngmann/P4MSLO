#!/usr/bin/env python3
"""Test 6-bit quantizer with Floyd-Steinberg dithering."""
import os, numpy as np
from PIL import Image
import random

SRC_DIR = os.path.dirname(os.path.abspath(__file__))
JPEGS = sorted([f for f in os.listdir(SRC_DIR) if f.endswith('.jpg')])
random.seed(42)
selected = random.sample(JPEGS, 4)
frames = [Image.open(os.path.join(SRC_DIR, f)).convert('RGB') for f in selected]

BITS = 6
CUBE = 1 << BITS
SHIFT = 8 - BITS

# Build 6-bit histogram from all frames (subsampled)
print(f"Building {BITS}-bit histogram ({CUBE}^3 = {CUBE**3} bins)...")
hist = np.zeros(CUBE**3, dtype=np.int64)
for img in frames:
    arr = np.array(img)[::4, ::4]  # subsample 4x for speed
    r = (arr[:,:,0] >> SHIFT).ravel().astype(np.int64)
    g = (arr[:,:,1] >> SHIFT).ravel().astype(np.int64)
    b = (arr[:,:,2] >> SHIFT).ravel().astype(np.int64)
    for i in range(len(r)):
        hist[r[i] * CUBE * CUBE + g[i] * CUBE + b[i]] += 1

# Median-cut in 6-bit space
print("Running median-cut...")
nonzero_indices = np.nonzero(hist)[0]
all_colors = []
for idx in nonzero_indices:
    r = (idx // (CUBE * CUBE)) % CUBE
    g = (idx // CUBE) % CUBE
    b = idx % CUBE
    all_colors.append((r, g, b, int(hist[idx])))

def median_cut(colors, n_colors):
    boxes = [colors]
    while len(boxes) < n_colors:
        # Find box with most volume (range * count)
        best_idx = -1
        best_score = -1
        for i, box in enumerate(boxes):
            if len(box) < 2:
                continue
            rs = [c[0] for c in box]
            gs = [c[1] for c in box]
            bs = [c[2] for c in box]
            rng = max(max(rs)-min(rs), max(gs)-min(gs), max(bs)-min(bs))
            cnt = sum(c[3] for c in box)
            score = rng * cnt
            if score > best_score:
                best_score = score
                best_idx = i
        if best_idx < 0:
            break

        box = boxes[best_idx]
        rs = [c[0] for c in box]
        gs = [c[1] for c in box]
        bs = [c[2] for c in box]
        rr = max(rs) - min(rs)
        gr = max(gs) - min(gs)
        br = max(bs) - min(bs)

        if rr >= gr and rr >= br:
            box.sort(key=lambda c: c[0])
        elif gr >= br:
            box.sort(key=lambda c: c[1])
        else:
            box.sort(key=lambda c: c[2])

        mid = len(box) // 2
        boxes[best_idx] = box[:mid]
        boxes.append(box[mid:])

    # Average each box to get palette entry
    palette = []
    for box in boxes:
        if not box:
            continue
        total = sum(c[3] for c in box)
        if total == 0:
            continue
        r = sum(c[0] * c[3] for c in box) / total
        g = sum(c[1] * c[3] for c in box) / total
        b = sum(c[2] * c[3] for c in box) / total
        # Scale 6-bit back to 8-bit
        r8 = min(int(r * (256 / CUBE) + 0.5), 255)
        g8 = min(int(g * (256 / CUBE) + 0.5), 255)
        b8 = min(int(b * (256 / CUBE) + 0.5), 255)
        palette.append((r8, g8, b8))

    while len(palette) < 256:
        palette.append((0, 0, 0))
    return palette[:256]

palette = median_cut(all_colors, 256)
pal_arr = np.array(palette, dtype=np.float64)
print(f"Palette: {len([p for p in palette if p != (0,0,0)])} colors")

# Create GIF with Floyd-Steinberg dithering
print("Encoding frames with Floyd-Steinberg dithering...")
gif_frames = []
for img in frames:
    arr = np.array(img, dtype=np.float64)
    h, w = arr.shape[:2]
    indices = np.zeros((h, w), dtype=np.uint8)

    for y in range(h):
        for x in range(w):
            # Current pixel (with accumulated error)
            r = max(0, min(255, arr[y, x, 0]))
            g = max(0, min(255, arr[y, x, 1]))
            b = max(0, min(255, arr[y, x, 2]))

            # Find nearest palette color
            dists = (pal_arr[:,0] - r)**2 + (pal_arr[:,1] - g)**2 + (pal_arr[:,2] - b)**2
            idx = np.argmin(dists)
            indices[y, x] = idx

            # Compute error
            er = r - palette[idx][0]
            eg = g - palette[idx][1]
            eb = b - palette[idx][2]

            # Distribute error (Floyd-Steinberg)
            if x + 1 < w:
                arr[y, x+1, 0] += er * 7/16
                arr[y, x+1, 1] += eg * 7/16
                arr[y, x+1, 2] += eb * 7/16
            if y + 1 < h:
                if x > 0:
                    arr[y+1, x-1, 0] += er * 3/16
                    arr[y+1, x-1, 1] += eg * 3/16
                    arr[y+1, x-1, 2] += eb * 3/16
                arr[y+1, x, 0] += er * 5/16
                arr[y+1, x, 1] += eg * 5/16
                arr[y+1, x, 2] += eb * 5/16
                if x + 1 < w:
                    arr[y+1, x+1, 0] += er * 1/16
                    arr[y+1, x+1, 1] += eg * 1/16
                    arr[y+1, x+1, 2] += eb * 1/16

        if y % 100 == 0:
            print(f"  Frame {len(gif_frames)+1}: row {y}/{h}")

    frame = Image.new('P', (w, h))
    flat_pal = []
    for pr, pg, pb in palette:
        flat_pal.extend([pr, pg, pb])
    frame.putpalette(flat_pal)
    frame.putdata(indices.ravel().tolist())
    gif_frames.append(frame)

out = os.path.join(SRC_DIR, "quality_6bit_dithered.gif")
gif_frames[0].save(out, save_all=True, append_images=gif_frames[1:], duration=300, loop=0)
print(f"\nSaved: {out} ({os.path.getsize(out)//1024}KB)")
print("Compare with quality_pillow_native.gif!")
