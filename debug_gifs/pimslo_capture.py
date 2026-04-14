#!/usr/bin/env python3
"""
PIMSLO Stereoscopic GIF Capture Pipeline

1. Triggers all 4 ESP32-S3 cameras via GPIO34 on the ESP32-P4
2. Downloads the latest photo from each camera via HTTP
3. Applies parallax cropping per camera position
4. Creates an oscillating 3D GIF (1→2→3→4→3→2→1)
"""

import os
import sys
import time
import serial
import requests
from PIL import Image
from io import BytesIO

# Camera IPs in position order (1-4)
CAMERAS = {
    1: "192.168.1.119",
    2: "192.168.1.248",
    3: "192.168.1.66",
    4: "192.168.1.38",
}

P4_SERIAL = "/dev/ttyACM1"
OUTPUT_DIR = os.path.dirname(os.path.abspath(__file__))
PARALLAX_STRENGTH = 0.2
GIF_FRAMERATE_MS = 150  # 150ms per frame

def trigger_cameras():
    """Pulse GPIO34 on the P4 to trigger all cameras simultaneously."""
    print("Triggering cameras via GPIO34...")
    ser = serial.Serial(P4_SERIAL, 115200, timeout=2)
    ser.dtr = True
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(b"trigger 200\r\n")
    ser.flush()
    time.sleep(1)
    data = ser.read(4096).decode("utf-8", errors="replace")
    ser.close()
    if "ok trigger" in data:
        print("  Trigger sent!")
    else:
        print(f"  Warning: unexpected response: {data[:100]}")

def download_latest_photo(position):
    """Download the latest photo from a camera."""
    ip = CAMERAS[position]
    url = f"http://{ip}/api/v1/latest-photo"
    print(f"  Downloading from camera {position} ({ip})...", end="", flush=True)
    try:
        resp = requests.get(url, timeout=30)
        if resp.status_code == 200:
            print(f" {len(resp.content)//1024}KB")
            return resp.content
        else:
            print(f" ERROR {resp.status_code}")
            return None
    except Exception as e:
        print(f" FAILED: {e}")
        return None

def apply_parallax_crop(img, position, total=4, strength=PARALLAX_STRENGTH):
    """Apply position-based horizontal parallax crop.

    Same algorithm as original PIMSLO (manual-gif.py crop_and_save_image).
    """
    w, h = img.size
    crop_ratio = (position - 1) / (total - 1)
    left_crop = int(crop_ratio * w * strength)
    right_crop = int((1 - crop_ratio) * w * strength)
    return img.crop((left_crop, 0, w - right_crop, h))

def create_pimslo_gif(images, output_path, frame_delay_ms=GIF_FRAMERATE_MS):
    """Create oscillating 3D GIF from 4 parallax-cropped images.

    Sequence: 1→2→3→4→3→2→1 (7 frames, oscillating)
    """
    # Apply parallax crop to each
    cropped = []
    for pos in range(1, 5):
        cropped_img = apply_parallax_crop(images[pos], pos)
        cropped.append(cropped_img)
        print(f"  Position {pos}: {images[pos].size} → {cropped_img.size}")

    # Ensure all frames are the same size (they should be by the math)
    target_size = cropped[0].size
    for i, img in enumerate(cropped):
        if img.size != target_size:
            print(f"  Warning: resizing frame {i+1} from {img.size} to {target_size}")
            cropped[i] = img.resize(target_size, Image.LANCZOS)

    # Build oscillating sequence: 1→2→3→4→3→2→1
    sequence = [cropped[0], cropped[1], cropped[2], cropped[3],
                cropped[2], cropped[1], cropped[0]]

    print(f"  Sequence: 7 frames at {frame_delay_ms}ms/frame")

    # Save as GIF
    sequence[0].save(
        output_path,
        save_all=True,
        append_images=sequence[1:],
        duration=frame_delay_ms,
        loop=0,
        optimize=False,
    )

    size = os.path.getsize(output_path)
    print(f"  Saved: {output_path} ({size//1024}KB)")
    return output_path

def main():
    print("=" * 50)
    print("PIMSLO Stereoscopic 3D GIF Capture")
    print("=" * 50)

    # Step 1: Get baseline photo counts
    print("\nBaseline photo counts:")
    before_counts = {}
    for pos, ip in CAMERAS.items():
        try:
            resp = requests.get(f"http://{ip}/api/v1/status", timeout=3)
            d = resp.json()
            before_counts[pos] = d.get("photo_count", 0)
            print(f"  Camera {pos} ({ip}): {before_counts[pos]} photos")
        except:
            before_counts[pos] = -1
            print(f"  Camera {pos} ({ip}): OFFLINE")

    # Step 2: Trigger all cameras
    trigger_cameras()

    # Step 3: Wait for captures to complete
    print("\nWaiting 3 seconds for cameras to save...")
    time.sleep(3)

    # Verify captures happened
    print("\nVerifying captures:")
    all_captured = True
    for pos, ip in CAMERAS.items():
        try:
            resp = requests.get(f"http://{ip}/api/v1/status", timeout=3)
            d = resp.json()
            after = d.get("photo_count", 0)
            delta = after - before_counts.get(pos, 0)
            status = "OK" if delta > 0 else "MISSED"
            if delta == 0:
                all_captured = False
            print(f"  Camera {pos}: {before_counts.get(pos,0)} → {after} ({'+' if delta > 0 else ''}{delta}) {status}")
        except:
            print(f"  Camera {pos}: OFFLINE")
            all_captured = False

    if not all_captured:
        print("\nWARNING: Not all cameras captured! Check wiring.")

    # Step 3: Download latest photo from each camera
    print("\nDownloading photos:")
    images = {}
    jpegs = {}
    for pos in range(1, 5):
        jpeg_data = download_latest_photo(pos)
        if jpeg_data is None:
            print(f"FATAL: Failed to download from camera {pos}")
            sys.exit(1)
        jpegs[pos] = jpeg_data
        img = Image.open(BytesIO(jpeg_data))
        # OV3660 is mounted inverted — rotate 180°
        images[pos] = img.rotate(180)

        # Save raw JPEG for reference
        raw_path = os.path.join(OUTPUT_DIR, f"pimslo_pos{pos}.jpg")
        with open(raw_path, "wb") as f:
            f.write(jpeg_data)

    print(f"\nAll 4 photos: {images[1].size[0]}x{images[1].size[1]}")

    # Step 4: Create stereoscopic GIF
    print("\nCreating 3D GIF:")
    timestamp = int(time.time())
    gif_path = os.path.join(OUTPUT_DIR, f"pimslo_{timestamp}.gif")
    create_pimslo_gif(images, gif_path)

    print(f"\n{'=' * 50}")
    print(f"Done! Open {gif_path} to see the 3D effect.")
    print(f"{'=' * 50}")

if __name__ == "__main__":
    main()
