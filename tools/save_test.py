#!/usr/bin/env python3
#
# frank-castle — save and reload on the board: warp into a save room (fg
# layer params & 0x20), stand in the middle so the coffin sequence starts,
# press UP to start it, confirm the save menu, reboot to the title and load the save from the
# memory card. Frames go to OUT/, steps are printed as they happen.
#
#   tools/save_test.py OUT [STAGE]
#
# SPDX-License-Identifier: AGPL-3.0-or-later
import os, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from castle_cmd import Castle, grab, PAD_CROSS, PAD_START, PAD_UP  # noqa: E402


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    out = a[0]
    stage = a[1] if len(a) > 1 else "nz0"
    os.makedirs(out, exist_ok=True)
    n = [0]

    def shot(tag):
        s = c.stat()
        grab(os.path.join(out, "%02d_%s.png" % (n[0], tag)))
        n[0] += 1
        print(tag, s, flush=True)
        return s

    c = Castle()
    m = c.mark()
    if not c.boot(stage):
        sys.exit("boot %s failed" % stage)
    rooms = [r for r in c.rooms() if len(r) > 7 and r[7] & 0x20]
    if not rooms:
        sys.exit("%s: no save room" % stage)
    room = rooms[0][0]
    print("save room", rooms[0], flush=True)
    c.go(stage, room, 0x80, 0x80)
    time.sleep(2)
    before = shot("in_room")
    # UP in front of the coffin starts it (Player_Stand_PressUp), it walks
    # the player to the middle and the menu asks to save; CROSS through it
    c.pad(PAD_UP, 20)
    time.sleep(3)
    print(c.cmd("save"), flush=True)
    for i in range(10):
        c.pad(PAD_CROSS, 6)
        time.sleep(1.5)
        shot("cross%d" % i)
        print(c.cmd("save"), flush=True)
    saved_hp = before and before.get("hp")

    # reboot to the title and load: START, then CROSS through the file menu
    if not c.boot_title():
        sys.exit("title boot failed")
    time.sleep(8)
    shot("title")
    for i, b in enumerate([PAD_START] + [PAD_CROSS] * 8):
        c.pad(b, 6)
        time.sleep(2)
        s = shot("load%d" % i)
        if s and s.get("state") == 2 and s.get("step") == 3:
            break
    time.sleep(3)
    s = shot("loaded")
    ok = bool(s and s.get("stage") == stage and s.get("room") == room)
    print("RESULT save/reload: %s (saved %s:%d hp %s, loaded %s)" % (
        "ok" if ok else "FAILED", stage, room, saved_hp,
        s and "%s:%s hp %s" % (s.get("stage"), s.get("room"), s.get("hp"))), flush=True)
    for p in c.problems(m):
        print("PROBLEM", p)


if __name__ == "__main__":
    main()
