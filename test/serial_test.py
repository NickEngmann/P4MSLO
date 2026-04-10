#!/usr/bin/env python3
"""
Serial test harness for ESP32-P4X-EYE automated testing.

Sends commands to the device's serial command interface and parses responses.
All responses from the device are prefixed with "CMD>" for easy parsing.

Usage:
    python3 serial_test.py                # Run all tests
    python3 serial_test.py ping           # Send a single command
    python3 serial_test.py status         # Check device status
"""

import serial
import time
import sys

PORT = '/dev/ttyACM0'
BAUD = 115200
TIMEOUT = 2.0
CMD_PREFIX = 'CMD>'

def open_serial():
    return serial.Serial(PORT, BAUD, timeout=TIMEOUT)

def send_command(ser, cmd, wait_secs=1.0):
    """Send a command and collect all CMD> prefixed responses."""
    # Flush any pending input
    ser.reset_input_buffer()

    # Send command
    ser.write((cmd + '\n').encode())
    ser.flush()

    # Collect responses
    responses = []
    start = time.time()
    while time.time() - start < wait_secs:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if line.startswith(CMD_PREFIX):
            resp = line[len(CMD_PREFIX):]
            responses.append(resp)
        elif line:
            # Log/debug output from ESP-IDF
            pass

    return responses

def wait_for_boot(ser, timeout=25):
    """Wait for the device to boot and the serial cmd interface to be ready."""
    start = time.time()
    while time.time() - start < timeout:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if 'CMD>ready' in line:
            print("[OK] Device serial command interface ready")
            return True
        if 'Application initialization completed' in line:
            # Wait a bit more for the serial task to start
            time.sleep(0.5)
    print("[WARN] Did not see 'ready' message, trying anyway...")
    return False

def test_ping(ser):
    """Test basic serial communication."""
    print("\n--- Test: ping ---")
    resp = send_command(ser, 'ping')
    if resp and 'pong' in resp[0]:
        print("[PASS] ping -> pong")
        return True
    else:
        print(f"[FAIL] ping -> {resp}")
        return False

def test_status(ser):
    """Test status command."""
    print("\n--- Test: status ---")
    resp = send_command(ser, 'status')
    if resp:
        print(f"[INFO] {resp[0]}")
        if 'page=' in resp[0] and 'sd=' in resp[0]:
            print("[PASS] status response parsed correctly")
            return True
    print(f"[FAIL] status -> {resp}")
    return False

def test_sd_ls(ser):
    """Test SD card directory listing."""
    print("\n--- Test: sd_ls ---")
    resp = send_command(ser, 'sd_ls /sdcard/esp32_p4_pic_save', wait_secs=3.0)
    if resp:
        for line in resp:
            print(f"  {line}")
        total_line = [r for r in resp if 'total=' in r]
        if total_line:
            print("[PASS] SD card listing works")
            return True
    print(f"[FAIL] sd_ls -> {resp}")
    return False

def test_goto_gifs(ser):
    """Test navigating to GIFs page."""
    print("\n--- Test: menu_goto gifs ---")
    resp = send_command(ser, 'menu_goto gifs', wait_secs=2.0)
    if resp and 'ok' in resp[0]:
        print("[PASS] Navigated to GIFs page")
        # Check status
        resp2 = send_command(ser, 'status')
        if resp2:
            print(f"  Status: {resp2[0]}")
        return True
    print(f"[FAIL] menu_goto gifs -> {resp}")
    return False

def test_gifs_create(ser):
    """Test GIF creation from album photos."""
    print("\n--- Test: gifs_create ---")
    # First make sure we're on a page that allows creation
    send_command(ser, 'menu_goto gifs', wait_secs=1.0)

    resp = send_command(ser, 'gifs_create 500', wait_secs=2.0)
    if resp and 'ok' in resp[0]:
        print("[PASS] GIF encoding started")

        # Poll for completion (encoding can take 10-60 seconds)
        print("  Waiting for encoding to complete...")
        for i in range(120):  # Up to 2 minutes
            time.sleep(1)
            resp = send_command(ser, 'status', wait_secs=1.0)
            if resp:
                status = resp[0]
                if 'gifs_encoding=0' in status:
                    print(f"  Encoding finished after ~{i+1}s")
                    return True
                elif 'gifs_encoding=1' in status:
                    if i % 10 == 0:
                        print(f"  Still encoding... ({i}s)")

        print("[FAIL] Encoding did not complete in 120s")
        return False
    print(f"[FAIL] gifs_create -> {resp}")
    return False

def test_gifs_verify(ser):
    """Verify the created GIF file exists and has correct format."""
    print("\n--- Test: gifs_verify ---")

    # List GIF directory
    resp = send_command(ser, 'sd_ls /sdcard/esp32_p4_gif_save', wait_secs=3.0)
    gif_files = [r for r in resp if '.gif' in r.lower() and 'FILE' in r]

    if not gif_files:
        print("[FAIL] No GIF files found")
        return False

    print(f"  Found {len(gif_files)} GIF file(s)")
    for f in gif_files:
        print(f"    {f}")

    # Get the first GIF filename
    # Parse: "  FILE animation_12345.gif size=1234"
    parts = gif_files[0].split()
    gif_name = parts[1] if len(parts) >= 2 else None
    if not gif_name:
        print("[FAIL] Cannot parse GIF filename")
        return False

    gif_path = f"/sdcard/esp32_p4_gif_save/{gif_name}"

    # Check file header
    resp = send_command(ser, f'sd_stat {gif_path}', wait_secs=2.0)
    if resp:
        print(f"  {resp[0]}")
        if 'type=GIF89a' in resp[0]:
            print("[PASS] GIF file has correct GIF89a header")

            # Hex dump first 32 bytes for verification
            resp2 = send_command(ser, f'sd_hexdump {gif_path} 0 32', wait_secs=2.0)
            if resp2:
                for line in resp2:
                    print(f"    {line}")
            return True
        else:
            print(f"[FAIL] GIF file has wrong header type")

    print(f"[FAIL] sd_stat failed")
    return False

def run_all_tests():
    """Run the full test suite."""
    ser = open_serial()

    # Wait for device to be ready
    print("Waiting for device boot...")
    time.sleep(3)  # Give device time to boot after flash

    # Flush any boot output
    ser.reset_input_buffer()

    results = {}
    results['ping'] = test_ping(ser)
    results['status'] = test_status(ser)
    results['sd_ls'] = test_sd_ls(ser)
    results['goto_gifs'] = test_goto_gifs(ser)
    results['gifs_create'] = test_gifs_create(ser)

    if results['gifs_create']:
        results['gifs_verify'] = test_gifs_verify(ser)

    # Return to main menu
    send_command(ser, 'menu_goto main')

    ser.close()

    print("\n" + "=" * 40)
    print("TEST RESULTS")
    print("=" * 40)
    passed = sum(1 for v in results.values() if v)
    total = len(results)
    for name, result in results.items():
        status = "PASS" if result else "FAIL"
        print(f"  [{status}] {name}")
    print(f"\n{passed}/{total} tests passed")

    return all(results.values())

if __name__ == '__main__':
    if len(sys.argv) > 1:
        # Single command mode
        cmd = ' '.join(sys.argv[1:])
        ser = open_serial()
        time.sleep(1)
        ser.reset_input_buffer()
        resp = send_command(ser, cmd, wait_secs=3.0)
        for line in resp:
            print(line)
        ser.close()
    else:
        success = run_all_tests()
        sys.exit(0 if success else 1)
