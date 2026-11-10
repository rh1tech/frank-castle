#!/usr/bin/env python3
# frank-castle — screenshots of the firmware's screens, with a flicker check
# of each: welcome, settings (on the welcome screen and in the game), the
# controls pages, the error screens.
# Usage: tools/shell_shots.py OUT_DIR
# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
from castle_cmd import Castle, grab, PAD_DOWN, PAD_UP, PAD_LEFT, PAD_CROSS, PAD_CIRCLE, PAD_RIGHT  # noqa: E402

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))


def flicker(name, secs):
    """record secs of HDMI in the background; returns the thread"""
    d = os.path.join(out, "rec_" + name)

    def run():
        r = subprocess.run([sys.executable, os.path.join(here, "flicker_check.py"), str(secs), d],
                           capture_output=True, text=True)
        with open(os.path.join(out, name + "_flicker.txt"), "w") as f:
            f.write(r.stdout + r.stderr)
        print(f"[{name}] " + (r.stdout.splitlines()[0] if r.stdout else r.stderr.strip()))
        for l in r.stdout.splitlines()[1:]:
            print(f"[{name}]   {l}")

    t = threading.Thread(target=run)
    t.start()
    return t


c = Castle()

# 1. the welcome screen, from a reboot
m = c.mark()
c._write(b"~RST~")
if not c.wait_for(r"frank-castle .* on M2", 10, m):
    sys.exit("no boot line")
t = flicker("welcome", 1.2)
# 2. the settings, opened from the welcome screen as soon as it says so
c.wait_for(r"Starting Castlevania", 15, m)
c._write(b"~C menu~")
c.wait_for(r"settings: menu open", 5, m)
t.join()
import shutil  # noqa: E402
_w = sorted(__import__("glob").glob(os.path.join(out, "rec_welcome", "f*.png")))
shutil.copy(_w[len(_w) // 2],
            os.path.join(out, "welcome.png"))
c._read(1.0)
t = flicker("settings", 4)
c._read(4.5)
t.join()
grab(os.path.join(out, "settings.png"))
# walk the pages: Effects' panel, the keyboard page, the gamepad page


def press(bits, wait=0.5):
    c.pad(bits, 3)
    c._read(wait)


press(PAD_DOWN)
press(PAD_RIGHT)  # into the Effects panel
grab(os.path.join(out, "effects.png"))
press(PAD_DOWN)
press(PAD_LEFT)
press(PAD_DOWN)
press(PAD_RIGHT)  # the keyboard page
grab(os.path.join(out, "keys.png"))
for _ in range(4):
    press(PAD_DOWN, 0.3)
grab(os.path.join(out, "keys_cross.png"))
press(PAD_LEFT)
press(PAD_DOWN)
press(PAD_RIGHT)  # the gamepad page
grab(os.path.join(out, "pad.png"))
press(PAD_LEFT)
for _ in range(3):
    press(PAD_UP, 0.3)
# 3. close: the game starts
c.pad(PAD_CIRCLE, 3)
c.wait_for(r"settings: menu closed", 5, m)
if not c.wait_for(r"\[perf\] \d+ fps", 40, m):
    print("game did not start")
c._read(6)
grab(os.path.join(out, "game_before.png"))
# 4. the menu in the game, and back
t = flicker("ingame_open", 4)
c._read(1.0)
c._write(b"~C menu~")
c.wait_for(r"settings: menu open", 5, m)
c._read(2.5)
t.join()
grab(os.path.join(out, "ingame_menu.png"))
t = flicker("ingame_static", 4)
c._read(4.5)
t.join()
t = flicker("ingame_nav", 8)
c._read(1.5)  # ffmpeg takes about a second to start recording
for bits in (PAD_DOWN, PAD_DOWN, PAD_RIGHT, PAD_DOWN, PAD_DOWN, PAD_LEFT, PAD_UP, PAD_UP):
    c.pad(bits, 3)
    c._read(0.5)
t.join()
t = flicker("ingame_close", 3)
c._read(0.8)
c.pad(PAD_CIRCLE, 3)
c._read(2.0)
t.join()
grab(os.path.join(out, "game_after.png"))
# 5. the error screens
for what in ("sd", "disc", "game"):
    c._write(("~C errtest %s~" % what).encode())
    c._read(1.5)
    t = flicker("err_" + what, 2)
    t.join()
    grab(os.path.join(out, "err_%s.png" % what))
    m2 = c.mark()
    c.wait_for(r"\[perf\] \d+ fps", 50, m2)
print("problems:", c.problems(m))
