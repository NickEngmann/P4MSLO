#!/usr/bin/env python3
"""Full-stack timing breakdown: button press → GIF on SD.

Records every latency the user cares about:
  W1   photo_btn → P4 photo saved to SD        (pimslo: Saved P4 preview)
  W2   pimslo trigger → SPI trigger sent       (spi_cam: Trigger sent)
  W3a  SPI trigger → first JPEG byte arrives   (spi_cam: JPEG ready, per cam)
  W3b  SPI transfer total                      (spi_cam: Camera N: X ms per cam)
  W4   Capture-all total                       (spi_cam: Capture all: N/4 ...)
  W5   each pos{N}.jpg SD save                 (pimslo: pos{N}: X bytes)
  W6   .p4ms saved                             (app_gifs: Direct-JPEG .p4ms saved)
  W7   GIF palette LUT build                   (gif_quant: Built/LUT built)
  W8   per-frame decode + encode               (gif_enc: Frame timing: ...)
  W9   GIF total                               (pimslo: GIF encode complete ... in Xs)

One photo capture → full encode cycle, printed as a single table."""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib


LOG = _lib.log_path(__file__)


def first_ts(txt, pattern, after_ts=0):
    for line in txt.splitlines():
        if re.search(pattern, line):
            m = re.match(r'\s*[IEWD]\s*\((\d+)\)', line)
            if m and int(m.group(1)) >= after_ts:
                return int(m.group(1))
    return None


def all_matches(txt, pattern, after_ts=0):
    out = []
    for line in txt.splitlines():
        m = re.search(pattern, line)
        if not m:
            continue
        ts_m = re.match(r'\s*[IEWD]\s*\((\d+)\)', line)
        if ts_m and int(ts_m.group(1)) >= after_ts:
            out.append((int(ts_m.group(1)), m))
    return out


def fmt(v, unit='ms'):
    if v is None:
        return '  —   '
    return f'{v:>5} {unit}'


def main():
    with open(LOG, 'w') as fh, _lib.open_port() as s:
        _lib.drain(s, 3, fh)
        _lib.do(s, 'ping', 1, fh)

        _lib.mark(fh, 'PHASE 1 — camera page')
        _lib.do(s, 'menu_goto camera', 5, fh)
        _lib.do(s, 'status', 1, fh)

        _lib.mark(fh, 'PHASE 2 — photo_btn + full capture')
        _lib.do(s, 'photo_btn', 3, fh)

        # Drain while capture completes (~4 s)
        _lib.mark(fh, 'PHASE 2b — wait for capture_all, then leave camera')
        t_end = time.time() + 20
        while time.time() < t_end:
            _lib.drain(s, 2, fh)
            with open(LOG) as lf:
                tx = lf.read()
            if re.search(r'Capture \d+: \d/4 cameras in', tx):
                break
        # Navigate off the camera page so the deferred encode can run
        _lib.do(s, 'menu_goto gifs', 3, fh)

        # Drain for up to 3 min to catch the entire capture + encode
        _lib.mark(fh, 'PHASE 3 — drain (up to 180s) for full GIF')
        t_end = time.time() + 180
        while time.time() < t_end:
            _lib.drain(s, 5, fh)
            with open(LOG) as lf:
                tx = lf.read()
            if 'GIF encode complete for P4M' in tx:
                break
        _lib.drain(s, 3, fh)
        _lib.do(s, 'status', 2, fh)

    with open(LOG) as f:
        txt = f.read()

    # Anchor = time of photo_btn press
    t0 = first_ts(txt, r"serial_cmd: Command: 'photo_btn'")
    after = (t0 or 0) + 1

    # Walk the full timeline
    t_preview     = first_ts(txt, r'pimslo: Saved P4 preview', after)
    t_trigger     = first_ts(txt, r'spi_cam: Trigger sent', after)
    # JPEG ready lines per camera — each one resets the 'JPEG ready' detection
    # so we get all of them. Capture their relative timestamps.
    cam_ready = all_matches(txt, r'spi_cam: JPEG ready: (\d+) bytes \(polled (\d+)ms\)', after)
    cam_perf  = all_matches(txt, r'spi_cam: Camera (\d+): (\d+) bytes in (\d+)ms', after)
    t_capture_all = first_ts(txt, r'spi_cam: Capture all: (\d+)/4 cameras, total (\d+)ms', after)
    cap_total_ms = None
    cap_n = None
    m = re.search(r'spi_cam: Capture all: (\d+)/4 cameras, total (\d+)ms', txt)
    if m:
        cap_n = int(m.group(1)); cap_total_ms = int(m.group(2))
    pos_saves = all_matches(txt, r'pimslo:\s+pos(\d+): (\d+) bytes', after)

    t_p4ms  = first_ts(txt, r'app_gifs: Direct-JPEG \.p4ms saved', after)

    t_lut   = first_ts(txt, r'gif_quant: LUT built', after)
    # Every frame in pass 2 prints a Frame timing line
    frames = all_matches(txt,
        r'gif_enc: Frame timing: decode=(\d+)ms encode=(\d+)ms total=(\d+)ms', after)

    t_gif_saved = first_ts(txt, r'app_gifs: PIMSLO GIF saved to', after)
    m2 = re.search(r'pimslo: GIF encode complete for (\S+) in (\d+)s', txt)
    gif_name = m2.group(1) if m2 else None
    gif_s    = int(m2.group(2)) if m2 else None

    def d(a, b):
        if a is None or b is None: return None
        return b - a

    w_p4_save = d(t0, t_preview)
    w_trigger = d(t_preview or t0, t_trigger)
    w_cap_all = cap_total_ms
    w_p4ms    = d(t_capture_all, t_p4ms)
    w_gif_total_ms = (gif_s * 1000) if gif_s is not None else None

    # Summary counters
    c = _lib.summarize(txt)

    print('=' * 70)
    print('  [11] DETAILED CAPTURE + ENCODE TIMING')
    print('=' * 70)
    print(f'  watchdogs      : {c["watchdogs"]}  ← MUST be 0')
    print(f'  panics         : {c["panics"]}  ← MUST be 0')
    print(f'  photos triggered: {c["photo_btn"]}')
    print(f'  captures       : {c["captures"]} (cams returning usable JPEGs)')
    print()
    print('  ── P4 photo side ──')
    print(f'   W1  photo_btn → P4 photo saved           : {fmt(w_p4_save)}')
    print(f'   W2  preview → SPI trigger sent           : {fmt(w_trigger)}')
    print()
    print(f'  ── SPI capture (4 cameras) ──')
    for ts, m in cam_perf:
        cam = int(m.group(1)); sz = int(m.group(2)); ms = int(m.group(3))
        print(f'   cam {cam}: {sz:>7} bytes in {ms:>5} ms  ({sz/ms:.1f} KB/s)')
    print(f'   W3  SPI capture-all total                : {fmt(w_cap_all)}')
    if cap_n is not None:
        print(f'        successful cameras                 : {cap_n}/4')
    print()
    print('  ── PSRAM + SD save ──')
    for ts, m in pos_saves:
        pos = int(m.group(1)); sz = int(m.group(2))
        print(f'   pos{pos}.jpg saved: {sz} bytes at t={ts}ms')
    print(f'   W4  capture_all → .p4ms saved            : {fmt(w_p4ms)}')
    print()
    print('  ── GIF encode ──')
    if t_lut is not None and t_capture_all is not None:
        print(f'   W5  palette LUT build (pass 1 + quant)   : {fmt(t_lut - t_capture_all)}')
    print(f'   W6  per-frame timings (pass 2):')
    for ts, m in frames:
        print(f'        decode={int(m.group(1)):>5}ms  encode={int(m.group(2)):>5}ms  total={int(m.group(3)):>5}ms')
    print(f'   W7  GIF total (pimslo timing)            : {fmt(w_gif_total_ms)}')
    if gif_name:
        print(f'        final file: /sdcard/p4mslo_gifs/{gif_name}.gif')
    print()
    print(f'  ── end-to-end ──')
    t_full = None
    if t0 is not None and t_gif_saved is not None:
        t_full = t_gif_saved - t0
    print(f'   TOTAL press → GIF on SD                   : {fmt(t_full)}')
    print('=' * 70)

    ok = (c['watchdogs'] == 0 and c['panics'] == 0 and
          c['photo_btn'] >= 1 and
          (cap_n or 0) >= 2 and
          t_gif_saved is not None)
    print(f"  VERDICT: {'PASS ✓' if ok else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
