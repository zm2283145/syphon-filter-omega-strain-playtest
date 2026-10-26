#!/bin/bash
# Port-only (132ps5c, research/PS5.md "Faults"): names link addresses (runtime - 0x400000) in the PS5 title.
# usage: sym.sh <llvm-pie.elf> <hex link address>...
E=$1; shift
llvm-nm-18 -n -C --defined-only "$E" | grep -E '^[0-9a-f]+ [tTwW] ' > /tmp/syms.txt
for a in "$@"; do
  python3 - "$a" <<'PY'
import sys, bisect
a = int(sys.argv[1], 16)
rows = []
for line in open('/tmp/syms.txt'):
    parts = line.rstrip('\n').split(' ', 2)
    rows.append((int(parts[0], 16), parts[2]))
keys = [r[0] for r in rows]
i = bisect.bisect_right(keys, a) - 1
print(hex(a), rows[i][1][:200] if i >= 0 else '?', '+' + hex(a - rows[i][0]) if i >= 0 else '')
PY
done
