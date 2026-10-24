#!/usr/bin/env python3
#
# frank-castle — fight a boss on the board: warp into its room with god
# mode on, cap the stage's entities at 1 HP, walk up to the one with the
# most HP and swing at it; frames are grabbed along the way.
#
#   tools/boss_test.py OUTDIR STAGE [ROOM] [--secs N] [--wait N]
#
# --wait: seconds to let the intro (cutscene) play before fighting.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import os, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from castle_cmd import Castle, grab, PAD_LEFT, PAD_RIGHT, PAD_SQUARE, PAD_CROSS  # noqa: E402


def opt(a, name, default):
    if name in a:
        i = a.index(name)
        v = a[i + 1]
        del a[i:i + 2]
        return float(v)
    return default


def main():
    a = sys.argv[1:]
    secs = opt(a, "--secs", 60)
    wait = opt(a, "--wait", 0)
    if len(a) < 2:
        sys.exit(__doc__)
    out, stage = a[0], a[1]
    room = int(a[2]) if len(a) > 2 else None
    os.makedirs(out, exist_ok=True)
    c = Castle()
    m = c.mark()
    if not c.boot(stage):
        sys.exit("boot failed")
    if room is not None:
        c.go(stage, room)
    c.cmd("god 1")
    t0 = time.time()
    n = 0
    while time.time() - t0 < wait:
        time.sleep(4)
        grab(os.path.join(out, "%s_%02d.png" % (stage, n)))
        n += 1
    t0 = last = time.time()
    side = 0
    while time.time() - t0 < secs:
        c.cmd("ehp 1")
        st = c.stat()
        ents = c.ents()
        if st and ents:
            # the boss is usually the entity with the lowest index
            tx = ents[0][2]
            dx = tx - st["x"]
            if abs(dx) > 40:
                c.pad(PAD_RIGHT if dx > 0 else PAD_LEFT, min(40, abs(dx) // 2))
            else:
                c.pad(PAD_RIGHT if dx > 0 else PAD_LEFT, 2)
                c.pad(PAD_SQUARE, 6)
                c.pad(0, 4)
                c.pad(PAD_CROSS | PAD_SQUARE, 12)
        else:
            # no boss yet: explore, turning round when blocked
            c.pad(PAD_LEFT if side else PAD_RIGHT, 60)
            st2 = c.stat()
            if st and st2 and abs(st2["x"] - st["x"]) < 8:
                side ^= 1
        if time.time() - last >= 5:
            last = time.time()
            grab(os.path.join(out, "%s_%02d.png" % (stage, n)))
            n += 1
    print(c.stat())
    for p in c.problems(m):
        print("PROBLEM", p)


if __name__ == "__main__":
    main()
