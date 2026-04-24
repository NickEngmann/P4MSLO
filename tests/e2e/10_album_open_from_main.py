#!/usr/bin/env python3
"""User-reported crash: from HOME, press btn_down (highlight ALBUM),
then btn_menu (select). Device hangs / resets.

Physical button path:
  iot_button PRESS_DOWN cb -> btn_handler -> bsp_display_lock ->
  ui_extra_btn_menu -> ui_extra_goto_page(ui_extra_get_choosed_page())
  -> ui_extra_redirect_to_gifs_page()

Serial path (what this test uses):
  cmd_btn("menu") -> bsp_display_lock -> ui_extra_btn_menu -> ...

Both should land at ui_extra_redirect_to_gifs_page. This test exercises
the full boot + press sequence 5 times over a fresh reset.

If it fails with a Guru Meditation or wdt, we have a real crash.
If it passes, the crash may be on hardware state specific to the
user's device (firmware version mismatch, SD content, etc.)."""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _lib

LOG = _lib.log_path(__file__)


def main():
    with open(LOG, 'w') as fh, _lib.open_port() as s:
        _lib.drain(s, 3, fh)
        _lib.do(s, 'ping', 1.5, fh)

        # 5 cycles of the user's exact flow. Between cycles we need to
        # scroll the main-menu selection back to the top (CAMERA) —
        # menu_goto main only changes the PAGE; the scroll position of
        # the home-menu container persists. We fire enough btn_up events
        # to scroll the menu all the way to the first item, then
        # btn_down ONCE to reach ALBUM.
        for i in range(5):
            _lib.mark(fh, f'CYCLE #{i+1}: home → up-to-top → down → menu → back')
            # Longer settle after menu_goto main — the first 1-2 btn_up
            # events otherwise race the main-menu paint on fresh boot
            # (the scroll-end callback that sets `selected_btn` only
            # fires once LVGL has finished the initial layout pass).
            # Observed pattern at 1 s settle: 2 of 5 cycles landed on
            # CAMERA instead of ALBUM; at 2 s the flake disappears.
            _lib.do(s, 'menu_goto main', 2, fh)
            # Scroll all the way up to CAMERA — send up 8x (more than
            # the 6 menu entries) so we're definitely at the top.
            for _ in range(8):
                _lib.do(s, 'btn_up', 0.35, fh)
            # Extra settle time so scroll-end callback sets selected_btn
            _lib.drain(s, 0.8, fh)

            # btn_down to highlight ALBUM (2nd item after CAMERA)
            _lib.do(s, 'btn_down', 1.2, fh)

            # btn_menu to select — user-reported crash trigger
            _lib.do(s, 'btn_menu', 6, fh)

            # Is device still alive?
            _lib.do(s, 'ping', 1.5, fh)
            _lib.do(s, 'status', 1.5, fh)

        # Final health check
        _lib.do(s, 'ping', 1.5, fh)

    with open(LOG) as f:
        txt = f.read()

    c = _lib.summarize(txt)
    panics = txt.count('Guru Meditation')
    reboots = txt.count('Rebooting') + txt.count('abort()')
    # Count how many times we reached GIFS page
    gifs_page_entries = sum(1 for line in txt.splitlines()
                            if 'page=GIFS' in line)
    pings_ok = txt.count('CMD>pong')

    _lib.print_summary('[10] Album open from main', c, extras={
        'panics (Guru)': panics,
        'reboots': reboots,
        'page=GIFS seen': gifs_page_entries,
        'pings_ok': pings_ok,
    })

    ok = (c['watchdogs'] == 0 and
          panics == 0 and
          reboots == 0 and
          pings_ok >= 6 and
          gifs_page_entries >= 3)
    print(f"  VERDICT: {'PASS ✓' if ok else 'FAIL ✗'}")
    print(f'  log: {LOG}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
