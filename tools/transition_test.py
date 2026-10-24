#!/usr/bin/env python3
#
# frank-castle — walk through every stage exit on the board: for each
# loading room (tilesetId 0xFF, the doorways to another stage) beside a
# room of the stage, warp next to the doorway, walk into it and see which
# stage loads. Results go to OUT.jsonl (one line per exit, resumable).
#
#   tools/transition_test.py OUT.jsonl [STAGE ...]
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import json, os, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from castle_cmd import Castle, grab, PAD_LEFT, PAD_RIGHT  # noqa: E402

STAGES = ("st0 no3 np3 nz0 nz1 no0 no1 no2 no4 lib dai are top cat chi cen dre "
          "rno0 rno1 rno2 rno3 rno4 rnz0 rnz1 rlib rdai rare rtop rcat rchi rcen").split()

# (stage, room, loading room) pairs that are walled off on the room's side:
# the loading room is shared with another room that does reach it, or (no4
# / rno4 37, the false save room) only an event opens the way to DRE
KNOWN_BLOCKED = {("no0", 15, 36), ("no1", 13, 20), ("no4", 37, 39), ("cen", 1, 4),
                 ("rno0", 15, 35), ("rno1", 13, 21), ("rno4", 37, 40), ("rcen", 1, 3)}


def exits(rooms):
    """(room, loading room, direction) for doorways reachable by walking"""
    out = []
    for lr in rooms:
        if lr[6] != 255:
            continue
        for r in rooms:
            if r[6] == 255 or not (r[2] <= lr[2] <= r[4]):
                continue
            if r[3] + 1 == lr[1]:
                out.append((r, lr, "right"))
            elif r[1] - 1 == lr[3]:
                out.append((r, lr, "left"))
    return out


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    out = a[0]
    stages = a[1:] or STAGES
    done = set()
    if os.path.exists(out):
        for l in open(out):
            r = json.loads(l)
            done.add((r["stage"], r["room"], r["loading_room"]))
    shots = os.path.splitext(out)[0] + "_shots"
    os.makedirs(shots, exist_ok=True)
    c = Castle()
    s = c.stat()
    if not s or s.get("state") != 2:
        c.boot("no3")
    c.cmd("god 1")
    log = open(out, "a")
    for st in stages:
        if not c.go(st, 0):
            c.boot(st)
            c.cmd("god 1")
        rooms = c.rooms()
        if not rooms:
            print("%-5s: no rooms (stage did not load)" % st, flush=True)
            continue
        for r, lr, d in exits(rooms):
            if (st, r[0], lr[0]) in done:
                continue
            # room pixels: the doorway's row, 48 px from the edge
            y = (lr[2] - r[2]) * 256 + 128
            x = (r[3] - r[1] + 1) * 256 - 48 if d == "right" else 48
            m = c.mark()
            c.cmd("near %s %d %d %d" % (st, r[0], x, y))
            time.sleep(3.5)
            before = c.stat()
            c.pad(PAD_RIGHT if d == "right" else PAD_LEFT, 150)
            end = time.time() + 15
            while True:  # the next stage loads, then plays
                time.sleep(1)
                after = c.stat()
                if time.time() > end or (after and after.get("stage") != st
                                         and after.get("step") == 3):
                    break
            rec = {"stage": st, "room": r[0], "loading_room": lr[0], "dir": d, "before": before,
                   "after": after, "problems": c.problems(m)}
            rec["changed"] = bool(after and after.get("stage") != st)
            rec["known_blocked"] = (st, r[0], lr[0]) in KNOWN_BLOCKED
            if not rec["changed"]:
                rec["shot"] = grab(os.path.join(shots, "%s_%02d_%02d.png" % (st, r[0], lr[0])))
            log.write(json.dumps(rec) + "\n")
            log.flush()
            print("%-5s room %2d -> loading %2d (%s): %s%s" % (
                st, r[0], lr[0], d,
                "%s:%s" % (after.get("stage"), after.get("room")) if after else "?",
                "" if rec["changed"] else
                " (known blocked)" if rec["known_blocked"] else " FAIL"), flush=True)
            if rec["problems"]:
                print("   PROBLEMS", rec["problems"])
                c.boot(st)
                c.cmd("god 1")


if __name__ == "__main__":
    main()
