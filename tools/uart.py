#!/usr/bin/env python3
#
# frank-castle — debug UART helper: capture the log and type pad input.
#
#   tools/uart.py cap SECONDS [OUT]          capture to OUT (default stdout)
#   tools/uart.py send "SEQ" [SECONDS OUT]   play SEQ, optionally capture meanwhile
#
# SEQ is space separated:  KEY | KEY*FRAMES (hold) | wMS (wait)
#   keys: w a s d (d-pad)  x z c v (cross square circle triangle)
#         q e 1 3 (L1 R1 L2 R2)  p (start)  o (select)
#   e.g.  "p w2000 p w1500 x*30"
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import os, select, sys, termios, threading, time

DEV = os.environ.get("CASTLE_UART", "/dev/cu.usbmodem11102")
FRAME = 1 / 60


def open_port():
    fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def capture(fd, secs, out, stop):
    end = time.time() + secs
    data = b""
    while time.time() < end and not stop.is_set():
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                data += os.read(fd, 4096)
            except BlockingIOError:
                pass
    text = data.replace(b"\r", b"").decode("utf-8", "replace")
    if out:
        open(out, "w").write(text)
    else:
        sys.stdout.write(text)


def play(fd, seq):
    for tok in seq.split():
        if tok[0] == "w" and tok[1:].isdigit():
            time.sleep(int(tok[1:]) / 1000)
            continue
        key, _, n = tok.partition("*")
        frames = int(n) if n else 1
        # the firmware holds a key 6 frames per byte; resend every 4 frames
        for _ in range(max(1, (frames + 3) // 4)):
            os.write(fd, key.encode())
            time.sleep(4 * FRAME)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__ if __doc__ else "usage: uart.py cap|send ...")
    fd = open_port()
    if sys.argv[1] == "cap":
        capture(fd, float(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else None, threading.Event())
    elif sys.argv[1] == "send":
        secs = float(sys.argv[3]) if len(sys.argv) > 3 else 0
        out = sys.argv[4] if len(sys.argv) > 4 else None
        stop = threading.Event()
        t = None
        if secs:
            t = threading.Thread(target=capture, args=(fd, secs, out, stop))
            t.start()
        play(fd, sys.argv[2])
        if t:
            t.join()


if __name__ == "__main__":
    main()
