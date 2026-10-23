#!/usr/bin/env python3
#
# frank-castle — visit every room of every stage on the board and poke at it.
#
#   tools/room_sweep.py OUTDIR [STAGE[:ROOMS] ...]
#
# For each room: warp in (tools/castle_cmd.py, no reflash), grab a frame,
# walk right and left, jump, attack, press up and down, grab another frame,
# and record what happened: fps, crashes, whether walking took the player
# into another room or stage (a transition), and whether the board still
# answers. A stuck board is rebooted and the sweep goes on.
#
# ROOMS: "3", "3-7" or "3,5,9"; default all. Default stages: everything the
# firmware builds. OUTDIR gets one PNG pair per room, sweep.jsonl (a line
# per room) and a contact sheet per stage (needs ImageMagick).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from castle_cmd import (Castle, grab, PAD_CROSS, PAD_DOWN, PAD_LEFT,  # noqa: E402
                        PAD_RIGHT, PAD_SQUARE, PAD_UP)

STAGES = ("no0 no1 lib cat no2 chi dai np3 cen no4 are top nz0 nz1 wrp no3 dre "
          "rno0 rno1 rlib rcat rno2 rchi rdai rno3 rcen rno4 rare rtop rnz0 rnz1 "
          "rwrp bo1 bo3 rbo0 rbo3 rbo4 rbo7").split()

# what the player does in each room: (pad bits, frames)
POKE = [
    (PAD_RIGHT, 60), (PAD_CROSS | PAD_RIGHT, 20), (PAD_RIGHT, 30), (PAD_SQUARE, 8), (0, 10),
    (PAD_LEFT, 90), (PAD_CROSS | PAD_LEFT, 20), (PAD_LEFT, 30), (PAD_SQUARE, 8), (0, 10),
    (PAD_UP, 20), (0, 10), (PAD_DOWN, 20), (0, 20),
]


def parse_rooms(spec, n):
    if not spec:
        return list(range(n))
    out = []
    for part in spec.split(","):
        a, _, b = part.partition("-")
        out += list(range(int(a), int(b) + 1)) if b else [int(a)]
    return [r for r in out if r < n]


def sweep_room(c, stage, room, outdir):
    rec = {"stage": stage, "room": room}
    m = c.mark()
    t0 = time.time()
    st = c.go(stage, room, settle=2.0)
    rec["warp_s"] = round(time.time() - t0, 1)
    if not st:
        rec["error"] = "warp failed"
        return rec
    rec["arrived"] = st
    rec["shot_in"] = grab(os.path.join(outdir, "%s_%02d_a.png" % (stage, room)))
    for bits, frames in POKE:
        c.pad(bits, frames)
    c._read(1.0)
    after = c.stat()
    rec["shot_out"] = grab(os.path.join(outdir, "%s_%02d_b.png" % (stage, room)))
    if not after:
        rec["error"] = "no answer after moving"
        return rec
    rec["after"] = after
    if (after.get("stage"), after.get("room")) != (stage, room):
        rec["moved_to"] = "%s:%s" % (after.get("stage"), after.get("room"))
    perf = c.perf(m)
    if perf:
        rec["fps_min"] = min(f for f, _ in perf[1:] or perf)
        rec["raster_max_us"] = max(r for _, r in perf)
    bad = c.problems(m)
    if bad:
        rec["problems"] = bad[:5]
    if after.get("dead"):
        rec["died"] = True
    return rec


def contact_sheet(outdir, stage, recs):
    tiles = []
    for r in recs:
        for k in ("shot_in", "shot_out"):
            if r.get(k) and os.path.exists(r[k]):
                tiles += ["-label", "%s %d%s" % (stage, r["room"], "" if k == "shot_in" else " after"), r[k]]
    if not tiles:
        return
    subprocess.run(["magick", "montage"] + tiles + ["-tile", "6x", "-geometry", "320x240+2+2",
                    "-pointsize", "14", os.path.join(outdir, "sheet_%s.png" % stage)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    plan = [a.partition(":")[::2] for a in sys.argv[2:]] or [(s, "") for s in STAGES]
    c = Castle()
    if not c.stat() or c.stat().get("state") != 2:
        c.boot(plan[0][0])
    c.cmd("god 1")
    log = open(os.path.join(outdir, "sweep.jsonl"), "a")
    for stage, spec in plan:
        if not c.go(stage, 0, settle=1.0):  # load the stage to read its rooms
            if not (c.boot(stage) and c.cmd("god 1")):
                print("%s: does not load" % stage, flush=True)
                log.write(json.dumps({"stage": stage, "error": "stage does not load"}) + "\n")
                continue
        rooms = parse_rooms(spec, len(c.rooms()))
        recs = []
        for room in rooms:
            rec = sweep_room(c, stage, room, outdir)
            if "error" in rec:  # wedged or lost: reboot into the stage, go on
                rec["rebooted"] = c.boot(stage) and c.cmd("god 1") is not None
            recs.append(rec)
            log.write(json.dumps(rec) + "\n")
            log.flush()
            note = " ".join(x for x in (
                rec.get("error", ""), "-> " + rec["moved_to"] if "moved_to" in rec else "",
                "%d fps" % rec["fps_min"] if "fps_min" in rec else "",
                "DIED" if rec.get("died") else "", "; ".join(rec.get("problems", []))) if x)
            print("%s %2d: %s" % (stage, room, note or "ok"), flush=True)
        contact_sheet(outdir, stage, recs)


if __name__ == "__main__":
    main()
