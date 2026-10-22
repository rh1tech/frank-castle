#!/usr/bin/env python3
# frank-castle — turn "[prof] <pc>" UART lines into a per-function profile.
# Usage: tools/profile.py LOG [ELF]
import collections, subprocess, sys

log = sys.argv[1]
elf = sys.argv[2] if len(sys.argv) > 2 else "build/frank-castle.elf"
pcs = [l.split()[1] for l in open(log, errors="replace") if l.startswith("[prof] ") and len(l.split()) == 2
       and all(c in "0123456789abcdef" for c in l.split()[1])]
if not pcs:
    sys.exit("no samples")
out = subprocess.run(["arm-none-eabi-addr2line", "-f", "-e", elf] + ["0x" + p for p in pcs],
                     capture_output=True, text=True).stdout.splitlines()
funcs = out[0::2]
files = out[1::2]
c = collections.Counter(funcs)
n = len(pcs)
where = collections.Counter()
for p in pcs:
    a = int(p, 16)
    where["SRAM" if 0x20000000 <= a < 0x20082000 else "flash" if 0x10000000 <= a < 0x11000000 else "other"] += 1
print(f"{n} samples; code in " + ", ".join(f"{k} {v*100//n}%" for k, v in where.items()))
for f, k in c.most_common(30):
    print(f"{k*100/n:5.1f}%  {f}")
