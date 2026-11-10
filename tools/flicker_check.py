#!/usr/bin/env python3
# frank-castle — record the HDMI capture for a while and report frames that
# differ from their neighbours inside a region (default: the shell's panel,
# leaving out the margins where the drips fall). A steady screen has no
# changed frames; a flicker shows as a frame that differs from both
# neighbours while those two agree.
# Usage: tools/flicker_check.py SECONDS OUT_DIR [x0 y0 x1 y1]  (640x480 coords)
# SPDX-License-Identifier: AGPL-3.0-or-later
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image

secs = float(sys.argv[1])
out = sys.argv[2]
box = tuple(int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else (40, 16, 600, 464)
os.makedirs(out, exist_ok=True)
for f in glob.glob(os.path.join(out, "f*.png")):
    os.remove(f)
subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "avfoundation",
                "-pixel_format", "uyvy422", "-framerate", "30", "-video_size", "640x480",
                "-i", "USB Video", "-t", str(secs), os.path.join(out, "f%04d.png")], check=True)
files = sorted(glob.glob(os.path.join(out, "f*.png")))
x0, y0, x1, y1 = box
frames = [np.asarray(Image.open(f).convert("L"), dtype=np.int16)[y0:y1, x0:x1] for f in files]
# capture noise: count pixels that moved by more than 40 levels
def diff(a, b):
    return int((np.abs(a - b) > 40).sum())
changes = [diff(frames[i], frames[i + 1]) for i in range(len(frames) - 1)]
flicker = []
for i in range(1, len(frames) - 1):
    if changes[i - 1] > 50 and changes[i] > 50 and diff(frames[i - 1], frames[i + 1]) <= 50:
        flicker.append(i)
print(f"{len(frames)} frames, changed transitions: {sum(c > 50 for c in changes)}, "
      f"flicker frames (differ from both neighbours that agree): {len(flicker)}")
for i, c in enumerate(changes):
    if c > 50:
        print(f"  {os.path.basename(files[i])} -> {os.path.basename(files[i + 1])}: {c} px")
if flicker:
    print("FLICKER at", [os.path.basename(files[i]) for i in flicker])
