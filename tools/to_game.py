#!/usr/bin/env python3
#
# frank-castle — drive a freshly booted board from the title screen into
# gameplay (New Game -> prologue) over the debug UART.
#
#   tools/to_game.py [TIMEOUT_S]
#
# Reads the "[perf] slot alloc X,Y WxH" lines to tell where the game is:
# 512 wide = title (press Start), 384 wide = menus/name entry (Cross, then
# Start), 256 wide = in game (done).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import os, re, select, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from uart import open_port, FRAME  # noqa: E402

ALLOC = re.compile(rb"slot alloc \d+,\d+ (\d+)x(\d+)")


def read_for(fd, secs):
    end, data = time.time() + secs, b""
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                data += os.read(fd, 4096)
            except BlockingIOError:
                pass
    return data


def press(fd, key, frames=6):
    for _ in range(max(1, (frames + 3) // 4)):
        os.write(fd, key.encode())
        time.sleep(4 * FRAME)


def main():
    timeout = float(sys.argv[1]) if len(sys.argv) > 1 else 120
    fd = open_port()
    end = time.time() + timeout
    while time.time() < end:
        widths = [int(m.group(1)) for m in ALLOC.finditer(read_for(fd, 2.5))]
        w = max(widths) if widths else 0
        if w == 256:  # only gameplay-sized targets in a whole perf window
            print("in game")
            return 0
        if w == 512:
            press(fd, "p")
        elif w == 384:
            press(fd, "x")
            read_for(fd, 1.5)
            press(fd, "p")
    print("timed out")
    return 1


if __name__ == "__main__":
    sys.exit(main())
