#!/usr/bin/env python3
"""
Trigger SPI captures and validate all 4 pos{1..4}.jpg with PIL.

Track per-camera success/failure over N runs. Camera 4 (pos4) has new
queued-SPI firmware; cameras 1-3 (pos1-3) still have the old sync code.
If the fix works, pos4 should pass much more reliably than pos1-3.
"""
import serial, time, base64, io, sys, re, os, argparse

P4_PORT = "/dev/ttyACM0"
BAUD = 115200

CHUNK = 4096


def open_port():
    s = serial.Serial(P4_PORT, BAUD, timeout=1)
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def drain(ser, wait=0.15):
    time.sleep(wait)
    while ser.in_waiting:
        ser.read(ser.in_waiting)
        time.sleep(0.05)


def send_cmd(ser, cmd, settle=0.1):
    drain(ser, 0.05)
    ser.write((cmd + "\n").encode())
    ser.flush()
    time.sleep(settle)


def read_until(ser, regex, timeout=30):
    deadline = time.time() + timeout
    buf = b""
    lines = []
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, _, buf = buf.partition(b"\n")
            text = line.decode(errors="replace").rstrip("\r")
            lines.append(text)
            if re.search(regex, text):
                return text, lines
    return None, lines


def trigger_capture(ser):
    send_cmd(ser, "spi_capture_all")
    match, _ = read_until(ser, r"ok spi_capture_all.*saved=", timeout=30)
    return match


def _strip_prompt(s):
    return re.sub(r"^CMD>\s*", "", s)


def _slurp_until(ser, terminator_re, timeout=5):
    """Read all bytes until a line matches terminator_re. Return full text."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            text_so_far = buf.decode("utf-8", errors="replace")
            for ln in text_so_far.splitlines():
                if re.search(terminator_re, ln):
                    # Keep reading a bit more in case there's trailing data on the same line
                    time.sleep(0.05)
                    extra = ser.read(ser.in_waiting) if ser.in_waiting else b""
                    return (buf + extra).decode("utf-8", errors="replace")
    return buf.decode("utf-8", errors="replace")


def download_file(ser, path):
    offset = 0
    data = bytearray()
    while True:
        send_cmd(ser, f"sd_base64 {path} {offset} {CHUNK}", settle=0.05)
        text = _slurp_until(ser, r"end_base64", timeout=5)

        # Find the header line with len=N
        m = re.search(r"base64\s+\S+\s+offset=\d+\s+len=(\d+)", text)
        if not m:
            break
        n = int(m.group(1))
        if n == 0:
            break

        # Extract b64 lines between the header and end_base64
        lines = [_strip_prompt(l.rstrip("\r")) for l in text.splitlines()]
        # Find indexes of header and terminator
        hdr_idx = None
        end_idx = None
        for i, l in enumerate(lines):
            if "base64 " in l and "len=" in l and hdr_idx is None:
                hdr_idx = i
            elif l == "end_base64":
                end_idx = i
                break

        if hdr_idx is None or end_idx is None:
            break

        b64 = ""
        for l in lines[hdr_idx + 1:end_idx]:
            if re.fullmatch(r"[A-Za-z0-9+/=]+", l):
                b64 += l

        try:
            decoded = base64.b64decode(b64)
        except Exception:
            break
        data.extend(decoded[:n])
        offset += n
        if n < CHUNK:
            break
    return bytes(data)


def validate(data):
    if len(data) < 4:
        return False, "too small"
    if data[:2] != b"\xff\xd8":
        return False, "no SOI"
    if data[-2:] != b"\xff\xd9":
        return False, "no EOI"
    try:
        from PIL import Image
        img = Image.open(io.BytesIO(data))
        img.load()
        return True, f"{img.size}"
    except Exception as e:
        return False, str(e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--save", action="store_true", help="save jpegs to /tmp/")
    args = ap.parse_args()

    ser = open_port()
    drain(ser, 0.3)

    stats = {1: [0, 0], 2: [0, 0], 3: [0, 0], 4: [0, 0]}  # [passes, fails]

    for run in range(1, args.runs + 1):
        print(f"\n=== Run {run}/{args.runs} ===")
        match = trigger_capture(ser)
        print(f"  capture: {match}")
        time.sleep(0.5)
        for cam in range(1, 5):
            path = f"/sdcard/pimslo/pos{cam}.jpg"
            data = download_file(ser, path)
            ok, msg = validate(data)
            mark = "PASS" if ok else "FAIL"
            print(f"  pos{cam}: {len(data):>7} bytes  {mark:4s}  {msg}")
            stats[cam][0 if ok else 1] += 1
            if args.save:
                with open(f"/tmp/run{run}_pos{cam}.jpg", "wb") as f:
                    f.write(data)

    print(f"\n=== Summary over {args.runs} runs ===")
    for cam in range(1, 5):
        p, f = stats[cam]
        fw = "NEW" if cam == 4 else "OLD"
        print(f"  pos{cam} ({fw} fw): {p} pass / {f} fail")

    ser.close()


if __name__ == "__main__":
    main()
