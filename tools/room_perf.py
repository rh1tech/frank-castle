#!/usr/bin/env python3
#
# frank-castle — measure rooms on their own, away from load dips.
#
#   tools/room_perf.py OUT.jsonl STAGE:ROOM [STAGE:ROOM ...]
#   tools/room_perf.py OUT.jsonl --from SWEEP.jsonl [--below FPS]
#
# Per room: warp in, wait for the load to settle, then take the perf
# window measured standing still and the one measured walking right (and
# left if that window was faster: the room may end on one side). Each
# window is ~2 s; the one taken is the last that lies wholly inside the
# phase. Rooms already in OUT.jsonl are skipped.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import json, os, re, sys

sys.path.insert(0, os.path.dirname(__file__))
from castle_cmd import Castle, PAD_LEFT, PAD_RIGHT  # noqa: E402

PERF = re.compile(r"\[perf\] (\d+) fps, game (\d+) us avg / (\d+) worst \| raster (\d+) us \((\d+) prims\)")


def window(c, mark):
    """the last perf window from mark on"""
    w = None
    for l in c.since(mark):
        g = PERF.search(l)
        if g:
            w = dict(zip(("fps", "game_us", "worst_us", "raster_us", "prims"), map(int, g.groups())))
    return w


def phase(c, bits, secs=4.6):
    m = c.mark()
    if bits:
        c.pad(bits, int(secs * 60))
    else:
        c._read(secs)
    return window(c, m)


def measure(c, stage, room):
    rec = {"stage": stage, "room": room}
    st = c.go(stage, room, settle=2.5)
    if not st:
        rec["error"] = "warp failed"
        return rec
    rec["idle"] = phase(c, 0)
    walks = []
    for bits in (PAD_RIGHT, PAD_LEFT):
        w = phase(c, bits)
        s = c.stat()
        if s and (s.get("stage"), s.get("room")) == (stage, room):
            walks.append(w)
        else:  # walked out: the window holds a room load, not this room
            rec.setdefault("walked_out", []).append("%s:%s" % (s.get("stage"), s.get("room")) if s else "?")
            if not c.go(stage, room, settle=2.5):
                break
    rec["walk"] = min((w for w in walks if w), key=lambda w: w["fps"], default=None)
    return rec


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    out = a[0]
    if a[1] == "--from":
        below = int(a[a.index("--below") + 1]) if "--below" in a else 50
        recs = {}
        for l in open(a[2]):
            r = json.loads(l)
            recs[(r["stage"], r.get("room"))] = r
        rooms = sorted(k for k, r in recs.items() if r.get("fps_min", 99) < below)
    else:
        rooms = [(s, int(r)) for s, _, r in (x.partition(":") for x in a[1:])]
    done = set()
    if os.path.exists(out):
        for l in open(out):
            r = json.loads(l)
            if "error" not in r:
                done.add((r["stage"], r["room"]))
    c = Castle()
    s = c.stat()
    if not s or s.get("state") != 2:
        c.boot(rooms[0][0])
    c.cmd("god 1")
    log = open(out, "a")
    for stage, room in rooms:
        if (stage, room) in done:
            continue
        rec = measure(c, stage, room)
        if "error" in rec:
            c.boot(stage)
            c.cmd("god 1")
        log.write(json.dumps(rec) + "\n")
        log.flush()
        i, w = rec.get("idle") or {}, rec.get("walk") or {}
        print("%-5s %2d: idle %2s fps (game %5s raster %5s us)  walk %2s fps (game %5s raster %5s us) %s" % (
            stage, room, i.get("fps"), i.get("game_us"), i.get("raster_us"),
            w.get("fps"), w.get("game_us"), w.get("raster_us"), rec.get("error", " ".join(rec.get("walked_out", [])))), flush=True)


if __name__ == "__main__":
    main()
