#!/usr/bin/env python3
#
# frank-castle — watch someone play: the UART goes to OUT/uart.log, the
# stage and room are polled and every change is logged with a frame grab,
# a frame is grabbed every --every seconds, and crashes are reported as
# they happen. With --playtest it turns the play-test mode on (god mode,
# enemies at 1 HP) and keeps it on across reboots.
#
#   tools/watch_play.py OUT [--playtest] [--every 30]
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import os, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from castle_cmd import Castle, grab, BAD  # noqa: E402


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    out = a[0]
    playtest = "--playtest" in a
    every = float(a[a.index("--every") + 1]) if "--every" in a else 30
    os.makedirs(out, exist_ok=True)
    events = open(os.path.join(out, "events.log"), "a")
    uart = open(os.path.join(out, "uart.log"), "a")

    def note(msg):
        line = "%s %s" % (time.strftime("%H:%M:%S"), msg)
        print(line, flush=True)
        events.write(line + "\n")
        events.flush()

    c = Castle()
    seen = c.mark()
    last_where, last_grab, last_poll, n = None, 0, 0, 0
    armed = False
    note("watching")
    while True:
        c._read(0.5)
        for l in c.log[seen:]:
            uart.write("%s %s\n" % (time.strftime("%H:%M:%S"), l))
            if any(b in l for b in BAD):
                note("PROBLEM " + l)
                grab(os.path.join(out, "%04d_problem.png" % n)); n += 1
            if "~ARGS?" in l:
                armed = False  # rebooted
        uart.flush()
        seen = len(c.log)
        now = time.time()
        if now - last_poll >= 5:
            last_poll = now
            s = c.stat()
            if s and s.get("state") == 2 and playtest and not armed:
                armed = c.cmd("playtest 1") is not None
                note("play-test mode on")
            where = s and (s.get("stage"), s.get("room"), s.get("state"))
            if s and where != last_where:
                note("stage %s room %s state %s at %s hp %s" % (
                    s.get("stage"), s.get("room"), s.get("state"), s.get("at"), s.get("hp")))
                last_where = where
                grab(os.path.join(out, "%04d_%s_%s.png" % (n, s.get("stage"), s.get("room")))); n += 1
                last_grab = now
        if now - last_grab >= every:
            last_grab = now
            grab(os.path.join(out, "%04d.png" % n)); n += 1


if __name__ == "__main__":
    main()
