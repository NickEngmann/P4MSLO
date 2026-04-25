"""Isolated encode-timing test.

Uses whatever JPEGs are already on SD in /sdcard/pimslo/pos{1..4}.jpg
(put there by a prior `spi_pimslo`). Runs ONLY the encode path
(`pimslo 150 0.05`) and measures wall-clock from command start to
"PIMSLO GIF saved to" log line.

If /sdcard/pimslo is empty, falls back to `spi_pimslo` first (which
does an SPI capture + save, then kicks off the encode — we then time
just the encode portion starting from `encoding...`).
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib


def main():
    port = os.environ.get('P4MSLO_TEST_PORT', '/dev/ttyACM0')
    log = os.path.join(os.path.dirname(__file__), '_isolate_encode.log')
    fh = open(log, 'w')
    s = _lib.open_port(port)

    _lib.mark(fh, 'BOOT + IDLE')
    _lib.drain(s, 2, fh)
    _lib.do(s, 'ping', 1, fh)
    _lib.reset_state(s, fh, timeout=30)

    # Drain any in-flight bg_worker encode — on fresh boot it'll try to
    # re-encode any /sdcard/p4mslo/ captures missing a .gif. That holds
    # s_ctx.is_encoding=true and makes our explicit `pimslo` command
    # return ESP_ERR_INVALID_STATE. Watch the log tail for 3 consecutive
    # "idle" snapshots (no new "Pass 2" / "PIMSLO GIF saved to" activity).
    _lib.mark(fh, 'WAIT BG IDLE')
    idle_checks = 0
    deadline = time.time() + 180
    last_saved = 0
    last_len = 0
    while time.time() < deadline:
        _lib.drain(s, 3, fh)
        fh.flush()
        with open(log) as lf:
            txt = lf.read()
        saved_count = txt.count('PIMSLO GIF saved to')
        cur_len = len(txt)
        if saved_count == last_saved and cur_len - last_len < 200:
            idle_checks += 1
            if idle_checks >= 3:
                break
        else:
            idle_checks = 0
        last_saved = saved_count
        last_len = cur_len
        time.sleep(2)

    _lib.mark(fh, 'LS /sdcard/pimslo')
    _lib.do(s, 'sd_ls /sdcard/pimslo', 2, fh)

    # Snapshot log tail to see if pos files exist
    fh.flush()
    with open(log) as lf:
        tail = lf.read()
    has_pos = all(f'pos{i}.jpg' in tail for i in range(1, 5))
    fh.write(f'\n# has_pos={has_pos}\n'); fh.flush()

    if not has_pos:
        print('[isolate_encode] /sdcard/pimslo missing pos files — running spi_pimslo to populate')
        _lib.mark(fh, 'spi_pimslo to populate')
        # Navigate to album page so encode doesn't defer
        _lib.do(s, 'menu_goto album', 1, fh)
        _lib.do(s, 'spi_pimslo', 10, fh)

    # Now time JUST the encode path.
    # Stay on MAIN page — going to ALBUM can kick the bg worker into
    # .p4ms pre-render which sets s_ctx.is_encoding=true and blocks our
    # explicit pimslo request with ESP_ERR_INVALID_STATE.
    _lib.mark(fh, 'ENCODE ISOLATION')
    _lib.do(s, 'menu_goto main', 1, fh)
    _lib.reset_state(s, fh, timeout=20)
    _lib.do(s, 'status', 1, fh)
    t0 = time.time()
    _lib.send(s, 'pimslo 150 0.05', fh)
    # Drain until we see PIMSLO GIF saved to, or 180 s hard stop (user
    # bar: encode must finish under 3 min or something is wrong — abort
    # the test so we can debug instead of burning more time).
    t_end = t0 + 180
    saved_ts = None
    p4ms_ts = None
    last_decode_times = []
    while time.time() < t_end:
        _lib.drain(s, 0.5, fh)
        fh.flush()
        with open(log) as lf:
            text = lf.read()
        if saved_ts is None and 'PIMSLO GIF saved to' in text:
            saved_ts = time.time() - t0
        if p4ms_ts is None and 'Direct-JPEG .p4ms saved' in text:
            p4ms_ts = time.time() - t0
        if saved_ts:
            break

    _lib.drain(s, 2, fh)
    fh.flush()
    with open(log) as lf:
        txt = lf.read()

    # Parse decode timing lines
    decode_lines = re.findall(r'save_small_gif_from_jpegs: cam (\d+) decode took (\d+)ms', txt)
    gif_elapsed = re.search(r'GIF encode complete.* in (\d+)s', txt)
    counters = _lib.summarize(txt)
    _lib.print_summary('ENCODE ISOLATION', counters, extras={
        'p4ms_elapsed_s': f'{p4ms_ts:.1f}' if p4ms_ts else 'n/a',
        'saved_elapsed_s': f'{saved_ts:.1f}' if saved_ts else 'n/a (timeout?)',
        'decode_per_cam_ms': ','.join(f'{c}:{t}' for c, t in decode_lines) or 'n/a',
        'gif_pipeline_s': gif_elapsed.group(1) if gif_elapsed else 'n/a',
    })
    ok = bool(saved_ts) and saved_ts < 80 and counters['panics'] == 0 and counters['watchdogs'] == 0
    print(f"  VERDICT: {'PASS' if ok else 'FAIL'}")
    s.close()
    fh.close()
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
