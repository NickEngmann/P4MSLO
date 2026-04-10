#!/usr/bin/env bash
# =============================================================
# Run this ONCE inside WSL2 to set up USB-serial permissions
# and verify usbip client tools are installed.
# =============================================================

set -e

echo "=== ESP32-P4 WSL2 Setup ==="
echo

# 1. Install usbip client tools
echo "[1/3] Installing USB/IP client tools..."
sudo apt-get update -qq
sudo apt-get install -y -qq linux-tools-generic hwdata 2>/dev/null || \
sudo apt-get install -y -qq linux-tools-virtual hwdata 2>/dev/null || \
echo "  ⚠  Could not auto-install linux-tools. You may need: sudo apt install linux-tools-$(uname -r)"

# usbip binary sometimes lands in a versioned path
if ! command -v usbip &>/dev/null; then
    USBIP_PATH=$(find /usr/lib/linux-tools/ -name usbip 2>/dev/null | head -1)
    if [ -n "$USBIP_PATH" ]; then
        sudo ln -sf "$USBIP_PATH" /usr/local/bin/usbip
        echo "  Symlinked usbip -> $USBIP_PATH"
    fi
fi

# 2. Add user to dialout so no sudo needed for /dev/ttyACM*
echo
echo "[2/3] Adding $USER to dialout group (avoids permission errors)..."
sudo usermod -aG dialout "$USER"

# 3. Verify
echo
echo "[3/3] Checking for attached ESP32 device..."
echo
ls -la /dev/ttyACM* /dev/ttyUSB* 2>/dev/null && echo "  ✓ Device(s) found above" \
    || echo "  No device yet — run the Windows .bat script first to attach."

echo
echo "=== Done ==="
echo "If you just got added to dialout, start a NEW WSL terminal for it to take effect."
