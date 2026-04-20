#!/usr/bin/env python3
"""
Run spi_pimslo N times, tally which pos{N}.jpg successfully decodes.

Parses P4 serial output for:
  - "ok spi_pimslo ... saved=K"  → how many cameras transferred cleanly structurally
  - "Pass 1 failed for pos N"    → which camera's JPEG tjpgd rejected
  - "Decoded 2560x1920 → ..."    → successful decode

Camera 4 (pos4) runs new queued-SPI firmware. Cameras 1-3 (pos1-3) have old.
If the fix works, pos4 should decode far more often than pos1-3.
"""
import serial, time, re, sys, argparse

P4_PORT = "/dev/ttyACM0"
BAUD = 115200


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--encode-timeout", type=int, default=70)
    args = ap.parse_args()

    ser = serial.Serial(P4_PORT, BAUD, timeout=0.2)
    time.sleep(0.3)
    ser.reset_input_buffer()

    # pos -> [pass, fail_at_mcu_list]
    results = {1: {"pass": 0, "fail_mcu": []},
               2: {"pass": 0, "fail_mcu": []},
               3: {"pass": 0, "fail_mcu": []},
               4: {"pass": 0, "fail_mcu": []}}
    saved_counts = []

    for run in range(1, args.runs + 1):
        print(f"\n=== Run {run}/{args.runs} ===")
        # Drain any stale serial
        time.sleep(0.2)
        ser.reset_input_buffer()

        # Trigger
        ser.write(b"spi_pimslo 150 0.05\n")
        ser.flush()

        # Track state: which pos we're currently decoding
        # Pass-1 log order is pos 1, 2, 3, 4 sequentially
        current_pos_decoding = 0  # becomes 1 when first "Loaded pos1" seen
        pos_outcome = {1: "?", 2: "?", 3: "?", 4: "?"}  # "pass"/"fail"
        fail_mcu = {1: None, 2: None, 3: None, 4: None}

        saved = None

        deadline = time.time() + args.encode_timeout
        buf = b""
        while time.time() < deadline:
            chunk = ser.read(ser.in_waiting or 1)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line_b, _, buf = buf.partition(b"\n")
                line = line_b.decode(errors="replace").rstrip("\r")

                m = re.search(r"ok spi_pimslo.*saved=(\d+)", line)
                if m:
                    saved = int(m.group(1))
                    print(f"  capture: saved={saved}")

                m = re.search(r"Loaded /sdcard/pimslo/pos(\d+)\.jpg:", line)
                if m:
                    current_pos_decoding = int(m.group(1))

                # Successful pass-1 decode:
                if "Decoded 2560x1920" in line:
                    if current_pos_decoding in (1, 2, 3, 4):
                        pos_outcome[current_pos_decoding] = "pass"

                # Failure: "Pass 1 failed for pos N"
                m = re.search(r"Pass 1 failed for pos (\d+)", line)
                if m:
                    pos_outcome[int(m.group(1))] = "fail"

                # MCU position where fail occurred
                m = re.search(r"mcu_load failed at MCU (\d+)/", line)
                if m and current_pos_decoding in (1, 2, 3, 4):
                    fail_mcu[current_pos_decoding] = int(m.group(1))

                # Detect "PIMSLO GIF saved" (done) or "Pass 2" (well underway, enough info)
                if "PIMSLO GIF saved" in line or "Pass 2 failed" in line:
                    deadline = time.time() - 1  # break outer loop

        if saved is not None:
            saved_counts.append(saved)
        for p in (1, 2, 3, 4):
            outcome = pos_outcome[p]
            if outcome == "pass":
                results[p]["pass"] += 1
                print(f"  pos{p}: PASS")
            elif outcome == "fail":
                results[p]["fail_mcu"].append(fail_mcu[p] or -1)
                print(f"  pos{p}: FAIL at MCU {fail_mcu[p]}")
            else:
                print(f"  pos{p}: ? (no info — camera may have not transferred)")

        # Give time between runs for everything to settle
        time.sleep(2)

    print("\n" + "=" * 50)
    print(f"Summary over {args.runs} runs (saved counts: {saved_counts}):")
    print("=" * 50)
    for p in (1, 2, 3, 4):
        r = results[p]
        fw = "NEW" if p == 4 else "OLD"
        mcus = r["fail_mcu"]
        mcu_str = f"MCUs failed: {mcus}" if mcus else ""
        print(f"  pos{p} ({fw} fw): {r['pass']} decode PASS / {len(r['fail_mcu'])} decode FAIL   {mcu_str}")

    ser.close()


if __name__ == "__main__":
    main()
