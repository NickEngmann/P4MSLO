#!/usr/bin/env python3
"""
Test different parallax strengths using the already-downloaded photos.
Creates multiple GIFs for comparison.
"""
import os
from PIL import Image
from io import BytesIO

SRC_DIR = os.path.dirname(os.path.abspath(__file__))

def load_photos():
    images = {}
    for pos in range(1, 5):
        path = os.path.join(SRC_DIR, f"pimslo_pos{pos}.jpg")
        img = Image.open(path).rotate(180)
        images[pos] = img
    print(f"Loaded 4 photos: {images[1].size}")
    return images

def apply_parallax_crop(img, position, total=4, strength=0.2):
    w, h = img.size
    crop_ratio = (position - 1) / (total - 1)
    left_crop = int(crop_ratio * w * strength)
    right_crop = int((1 - crop_ratio) * w * strength)
    return img.crop((left_crop, 0, w - right_crop, h))

def create_gif(images, strength, label):
    cropped = []
    for pos in range(1, 5):
        c = apply_parallax_crop(images[pos], pos, strength=strength)
        cropped.append(c)

    # Ensure same size
    min_w = min(c.size[0] for c in cropped)
    cropped = [c.crop((0, 0, min_w, c.size[1])) for c in cropped]

    # Oscillating: 1→2→3→4→3→2→1
    seq = [cropped[0], cropped[1], cropped[2], cropped[3],
           cropped[2], cropped[1], cropped[0]]

    path = os.path.join(SRC_DIR, f"pimslo_parallax_{label}.gif")
    seq[0].save(path, save_all=True, append_images=seq[1:],
                duration=150, loop=0, optimize=False)
    print(f"  {label}: strength={strength:.2f}, size={cropped[0].size}, {os.path.getsize(path)//1024}KB")
    return path

images = load_photos()

print("\nCreating comparison GIFs:")
create_gif(images, 0.00, "none")       # No software parallax (pure camera displacement)
create_gif(images, 0.05, "subtle")     # Very subtle
create_gif(images, 0.10, "low")        # Low
create_gif(images, 0.20, "normal")     # Original PIMSLO default
create_gif(images, 0.30, "high")       # Strong

print("\nCompare these to find the best 3D effect for your camera spacing:")
print("  pimslo_parallax_none.gif    — pure natural parallax")
print("  pimslo_parallax_subtle.gif  — 5% crop")
print("  pimslo_parallax_low.gif     — 10% crop")
print("  pimslo_parallax_normal.gif  — 20% (original PIMSLO)")
print("  pimslo_parallax_high.gif    — 30% crop")
