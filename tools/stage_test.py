#!/usr/bin/env python3
#
# frank-castle — boot straight into a stage and collect perf lines.
#
#   tools/stage_test.py STAGE [SECONDS] [KEYS]
#
# Reboots the board ("~RST~"), passes "--stage STAGE" during the boot
# window ("~ARGS ...~"), waits for gameplay, then plays KEYS (tools/uart.py
# send syntax) while logging. Prints fps and worst-frame per perf window.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import os, re, select, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from uart import open_port, play  # noqa: E402


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


def main():
    stage = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 12
    keys = sys.argv[3] if len(sys.argv) > 3 else ""
    fd = open_port()
    os.write(fd, b"~RST~")
    log = b""
    end = time.time() + 6
    while time.time() < end:  # spam the args until the firmware echoes them
        os.write(fd, ("~ARGS --stage %s~" % stage).encode())
        log += read_for(fd, 0.25)
        if b"args from UART" in log:
            break
    log += read_for(fd, 14)  # boot, load
    import threading
    t = threading.Thread(target=play, args=(fd, keys)) if keys else None
    if t:
        t.start()
    log += read_for(fd, secs)
    if t:
        t.join()
    text = log.decode("utf-8", "replace").replace("\r", "")
    for l in text.splitlines():
        if "fps" in l and l.startswith("[perf]"):
            m = re.search(r"(\d+) fps, game \d+ us avg / (\d+) worst \| raster (\d+) us \((\d+) prims\)", l)
            if m:
                print("%-5s %s fps  worst %5.1f ms  raster %5.1f ms  %s prims" % (
                    stage, m.group(1), int(m.group(2)) / 1000, int(m.group(3)) / 1000, m.group(4)))
        elif any(k in l for k in ("PANIC", "HARDFAULT", "FATAL", "not found", "failed")):
            print(stage, "!!", l[:150])


if __name__ == "__main__":
    main()
