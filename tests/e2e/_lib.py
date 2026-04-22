"""Shared helpers for on-device end-to-end tests.

All tests open /dev/ttyACM0, write serial commands (matching app_serial_cmd.c),
capture the device's log stream, and extract pass/fail signals from it.

A test lives as a single Python file in this directory. The test should:

1. `import _lib` and open a serial port via `_lib.open_port()`
2. Use `_lib.do(s, cmd, wait, fh)` to send a CMD and capture output for `wait` seconds
3. Use `_lib.drain(s, dur, fh)` to just capture output without sending
4. At the end, parse the log and print a human-readable report + a
   single-line PASS/FAIL verdict.

Logs are written alongside the test script as `<test>.log` so
subsequent runs preserve the artifact for debugging.
"""
import os
import re
import serial
import time

DEFAULT_PORT = '/dev/ttyACM0'
DEFAULT_BAUD = 115200


def open_port(port=DEFAULT_PORT, baud=DEFAULT_BAUD):
    """Open the P4 serial port with our standard timeouts."""
    return serial.Serial(port, baud, timeout=0.1)


def drain(s, dur, fh):
    """Read + record serial output for `dur` seconds."""
    t_end = time.time() + dur
    while time.time() < t_end:
        try:
            d = s.read(8192)
        except serial.SerialException:
            return
        if d:
            fh.write(d.decode('utf-8', 'replace'))
            fh.flush()


def send(s, cmd, fh):
    """Write a command to the device + mark it in the log.

    We do NOT flush the input buffer before sending — if the board is
    busy with a prior long-running op (SPI capture can take 20+ s when
    S3 cams retry), a prior command's response may still be streaming
    and dropping it would break the log. Each individual `do` call
    drains for `wait` seconds after send, and that drain captures the
    response as long as it arrives within the window."""
    s.write((cmd + '\n').encode())
    s.flush()
    fh.write(f'\n=== {cmd} ===\n')
    fh.flush()


def do(s, cmd, wait, fh):
    """send() + drain(). Returns nothing — inspect the log at the end."""
    send(s, cmd, fh)
    drain(s, wait, fh)


def wait_for_idle(s, fh, max_wait=120):
    """Poll `status` until the board reports pimslo_encoding=0 AND
    pimslo_queue=0 OR pimslo_queue>0 with page!=CAMERA (meaning encode
    is deferred by page, not actively running). Useful between phases
    so we don't send commands while an SPI capture is in flight."""
    import re
    t_end = time.time() + max_wait
    while time.time() < t_end:
        s.write(b'status\n')
        s.flush()
        t_drain = time.time() + 2.5
        buf = b''
        while time.time() < t_drain:
            d = s.read(2048)
            if d:
                buf += d
                if b'CMD>' in buf:
                    break
        txt = buf.decode('utf-8', 'replace')
        fh.write(txt)
        fh.flush()
        m = re.search(r'fw=.* pimslo_queue=(\d+) pimslo_encoding=(\d+)', txt)
        if m:
            q, enc = int(m.group(1)), int(m.group(2))
            if q == 0 and enc == 0:
                return True
        time.sleep(1)
    return False


def mark(fh, tag):
    """Insert a human-readable section marker into the log."""
    fh.write(f'\n########## {tag} ##########\n')
    fh.flush()


def log_path(test_script_path):
    """<test>.log in the same dir as the test script."""
    base = os.path.splitext(os.path.basename(test_script_path))[0]
    return os.path.join(os.path.dirname(test_script_path), f'{base}.log')


def summarize(txt):
    """Common counters every test surfaces."""
    return {
        'watchdogs': txt.count('task_wdt'),
        'panics': txt.count('Guru Meditation'),
        'ping_pong': txt.count('CMD>pong'),
        'photo_btn': txt.count('photo_btn (capture queued)'),
        'captures': re.findall(r'Capture \d+: (\d)/4 cameras', txt),
        'p4ms_saved': txt.count('Direct-JPEG .p4ms saved'),
        'gifs_saved': txt.count('PIMSLO GIF saved to'),
        'encode_defers': txt.count('Deferring'),
        'bg_encodes': txt.count('BG: encoding PIMSLO'),
        'bg_prerenders': txt.count('BG: pre-render success'),
        'deletes': txt.count('Deleted gallery entry'),
        'cached_loads': txt.count('Loaded small GIF'),
        'oom': txt.count('OOM') + txt.count('NO_MEM'),
        'realloc_fail': txt.count('Failed to reallocate'),
    }


def print_summary(title, counters, extras=None):
    print('\n' + '=' * 48)
    print(f'  {title}')
    print('=' * 48)
    for k in ['watchdogs', 'panics']:
        flag = '  ← MUST be 0' if counters[k] != 0 else '  ✓'
        print(f'  {k:<16}: {counters[k]}{flag}')
    for k in ['ping_pong', 'photo_btn', 'p4ms_saved', 'gifs_saved',
              'encode_defers', 'bg_encodes', 'bg_prerenders',
              'deletes', 'cached_loads', 'oom', 'realloc_fail']:
        print(f'  {k:<16}: {counters[k]}')
    print(f"  captures        : {counters['captures']}")
    if extras:
        print()
        for k, v in extras.items():
            print(f'  {k:<16}: {v}')
    print('=' * 48)


def verdict(counters, require_gifs=0, require_p4ms=0):
    """Standard pass criteria: no crashes, optional min-counts for encode."""
    ok = (counters['watchdogs'] == 0 and
          counters['panics'] == 0 and
          counters['ping_pong'] >= 1 and
          counters['gifs_saved'] >= require_gifs and
          counters['p4ms_saved'] >= require_p4ms)
    print(f"  VERDICT: {'PASS ✓' if ok else 'FAIL ✗'}")
    return ok
