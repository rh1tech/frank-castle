#!/usr/bin/env python3
#
# frank-castle — drive the game over the debug UART (src/pc/debug_cmd.c).
#
#   tools/castle_cmd.py boot STAGE           reboot into STAGE (no reflash)
#   tools/castle_cmd.py go STAGE ROOM [X Y]  warp at runtime; prints STAT after
#   tools/castle_cmd.py stat | rooms | god 0|1
#   tools/castle_cmd.py pad BITS FRAMES      hold pad bits (hex) for FRAMES
#
# As a module: Castle() keeps the port open, collects the log (perf lines,
# FATAL/HARDFAULT) and answers commands. Pad bits: PAD_* below.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import os, re, select, subprocess, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from uart import open_port  # noqa: E402

PAD_L2, PAD_R2, PAD_L1, PAD_R1 = 1 << 0, 1 << 1, 1 << 2, 1 << 3
PAD_TRIANGLE, PAD_CIRCLE, PAD_CROSS, PAD_SQUARE = 1 << 4, 1 << 5, 1 << 6, 1 << 7
PAD_SELECT, PAD_START = 1 << 8, 1 << 11
PAD_UP, PAD_RIGHT, PAD_DOWN, PAD_LEFT = 1 << 12, 1 << 13, 1 << 14, 1 << 15
FRAME = 1 / 60
BAD = ("HARDFAULT", "PANIC", "FATAL", "*** PREVIOUS CRASH")


class Castle:
    def __init__(self):
        self.fd = open_port()
        self.buf = b""
        self.log = []  # every complete line seen

    def _read(self, secs):
        end = time.time() + secs
        while True:
            r, _, _ = select.select([self.fd], [], [], max(0, min(0.05, end - time.time())))
            if r:
                try:
                    self.buf += os.read(self.fd, 4096)
                except BlockingIOError:
                    pass
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                self.log.append(line.decode("utf-8", "replace").rstrip("\r"))
            if time.time() >= end:
                return

    def mark(self):
        """position in the log, for since()"""
        self._read(0)
        return len(self.log)

    def since(self, mark):
        self._read(0)
        return self.log[mark:]

    def wait_for(self, pattern, secs, mark=None):
        """first line from mark on matching pattern, or None"""
        mark = self.mark() if mark is None else mark
        end = time.time() + secs
        while time.time() < end:
            self._read(0.05)
            for l in self.log[mark:]:
                if re.search(pattern, l):
                    return l
        return None

    def boot(self, stage, secs=30):
        """reboot and start STAGE (extra --args allowed); True once in play"""
        m = self.mark()
        os.write(self.fd, b"~RST~")
        if not self.wait_for(r"~ARGS\?", 8, m):
            return False
        os.write(self.fd, ("~ARGS --stage %s~" % stage).encode())
        if self.wait_for(r"\[perf\] \d+ fps", secs, m) is None:
            return False
        end = time.time() + secs
        while time.time() < end:  # loaded and playing
            s = self.stat()
            if s and s.get("state") == 2 and s.get("step") == 3:
                return True
            self._read(0.5)
        return False

    def cmd(self, line, secs=3):
        """send a debug command; its "~R ..." reply (without the prefix)"""
        m = self.mark()
        os.write(self.fd, ("~C %s~" % line).encode())
        r = self.wait_for(r"^~R ", secs, m)
        return r[3:] if r else None

    def rooms(self):
        m = self.mark()
        os.write(self.fd, b"~C rooms~")
        if not self.wait_for(r"^~R ROOMS \d+", 5, m):
            return []
        out = []
        for l in self.since(m):
            g = re.match(r"~R ROOM (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)", l)
            if g:
                out.append(tuple(int(x) for x in g.groups()))
        return out

    def stat(self):
        r = self.cmd("stat")
        if not r or not r.startswith("STAT"):
            return None
        return {k: (int(v) if re.fullmatch(r"-?\d+", v) else v)
                for k, v in re.findall(r"(\w+)=(\S+)", r)}

    def go(self, stage, room, x=None, y=None, settle=4.0):
        """warp; the STAT once the room is up (None if it did not get there)"""
        r = self.cmd("go %s %d" % (stage, room) + ("" if x is None else " %d %d" % (x, y)))
        if not r or not r.startswith("OK"):
            return None
        end = time.time() + 20
        while time.time() < end:  # reload: wait for the stage, then the room
            self._read(0.3)
            s = self.stat()
            if s and s.get("stage") == stage and s.get("room") == room and s.get("step") == 3:
                self._read(settle)
                return self.stat()
        return None

    def pad(self, bits, frames):
        """hold pad bits for frames (blocks for that long)"""
        os.write(self.fd, ("~C pad %x %d~" % (bits, frames)).encode())
        self._read(frames * FRAME + 0.05)

    def problems(self, mark=0):
        return [l for l in self.log[mark:] if any(b in l for b in BAD)]

    def perf(self, mark=0):
        """(fps, raster us) per perf window since mark"""
        out = []
        for l in self.log[mark:]:
            g = re.search(r"\[perf\] (\d+) fps, game \d+ us avg / \d+ worst \| raster (\d+) us", l)
            if g:
                out.append((int(g.group(1)), int(g.group(2))))
        return out


def grab(path):
    """one HDMI frame from the capture card (tools/grab.sh)"""
    subprocess.run([os.path.join(os.path.dirname(__file__), "grab.sh"), path],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return path


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    c = Castle()
    if a[0] == "boot":
        print("ok" if c.boot(" ".join(a[1:])) else "failed")
    elif a[0] == "go":
        x, y = (int(a[3]), int(a[4])) if len(a) >= 5 else (None, None)
        print(c.go(a[1], int(a[2]), x, y))
    elif a[0] == "rooms":
        for r in c.rooms():
            print("room %d: x %d-%d y %d-%d layout %d" % (r[0], r[1], r[3], r[2], r[4], r[5]))
    elif a[0] == "pad":
        c.pad(int(a[1], 16), int(a[2]))
    else:
        print(c.cmd(" ".join(a)))


if __name__ == "__main__":
    main()
