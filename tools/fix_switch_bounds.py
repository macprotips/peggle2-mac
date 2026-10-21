"""Write explicit bounds for frameless functions (no .pdata entry) that contain jump tables.

XenonAnalyse sizes those functions itself and stops at the bctr, cutting off the switch cases.

usage: fix_switch_bounds.py image.bin switch_tables.toml out.toml
"""
import bisect, re, struct, sys

img = open(sys.argv[1], 'rb').read()
BASE, TEXT, TEXT_END = 0x82000000, 0x82140000, 0x828AF6A8
w = lambda a: struct.unpack_from('>I', img, a - BASE)[0]

tables = {}
for blk in open(sys.argv[2]).read().split('[[switch]]')[1:]:
    base = int(re.search(r'base = 0x([0-9A-F]+)', blk)[1], 16)
    tables[base] = [int(x, 16) for x in re.findall(r'0x([0-9A-F]+),', blk)]

def terminator(x):
    return x == 0x4E800020 or x == 0x4E800420 or (x >> 26 == 18 and x & 3 == 0)  # blr, bctr, b

# Real entry points: .pdata starts and bl targets.
entries, pdata = set(), []
for o in range(0, 0x30F50, 8):
    s, packed = struct.unpack_from('>II', img, 0x82105600 - BASE + o)
    if s:
        entries.add(s)
        pdata.append((s, s + ((packed >> 8) & 0x3FFFFF) * 4))
in_pdata = lambda a: any(b <= a < e for b, e in pdata)
for a in range(TEXT, TEXT_END, 4):
    x = w(a)
    if x >> 26 == 18 and x & 3 == 1:
        d = x & 0x3FFFFFC
        entries.add(a + (d - 0x4000000 if d & 0x2000000 else d))
# Functions reached only through pointers (vtables, callbacks, comparators) are entry points too.
# A data word counts only if it is preceded by a terminator or padding, so a stray constant that
# happens to point into the middle of a function cannot split it.
DATA = [(0x82000600, 0x82105600), (0x828B6000, 0x82D990F8)]
for lo, hi in DATA:
    for a in range(lo, hi, 4):
        v = w(a)
        if TEXT <= v < TEXT_END and v % 4 == 0 and (terminator(w(v - 4)) or w(v - 4) == 0):
            entries.add(v)
ends = sorted(entries)
# Starts also include the first instruction after alignment padding (frameless functions).
starts = sorted(entries | {a + 4 for a in range(TEXT, TEXT_END, 4) if w(a) == 0 and w(a + 4) != 0})


bounds = {}
for tbase, labels in sorted(tables.items()):
    if in_pdata(tbase):
        continue
    start = starts[bisect.bisect_right(starts, tbase) - 1]
    # Walk forward from the last case to the first unconditional terminator not skipped over by a
    # forward branch. Functions reached only through pointers (eg. qsort comparators) can follow
    # directly, so the next known entry point is only an upper limit.
    limit = ends[bisect.bisect_right(ends, max(labels))]
    a, reach = max(labels), max(labels)
    while a < limit:
        x = w(a)
        if x >> 26 == 16 and not (x & 2):                      # conditional branch: may jump further
            d = x & 0xFFFC
            reach = max(reach, a + (d - 0x10000 if d & 0x8000 else d))
        if terminator(x) and a >= reach:
            break
        a += 4
    end = min(a + 4, limit)
    while w(end - 4) == 0: end -= 4  # alignment padding
    bounds[start] = max(bounds.get(start, 0), end)

# Functions the analyzer cuts at a conditional return, losing the code that follows (found by checking for
# functions whose last instruction can fall through).
MANUAL = {
    0x825F10B0: 0x825F1150,  # CRT memset: cut at a bdzlr, so 2- and 3-byte tails were left unwritten
}
for start, end in MANUAL.items():
    bounds[start] = max(bounds.get(start, 0), end)

out, problems = [], []
for start, end in sorted(bounds.items()):
    if out and start < out[-1][0] + out[-1][1]:  # overlapping: merge into the previous function
        prev = out.pop(); start = prev[0]; end = max(end, prev[0] + prev[1])
    out.append((start, end - start))
for start, size in out:
    if not terminator(w(start + size - 4)): problems.append('%X..%X ends with %08X' % (start, start + size, w(start + size - 4)))

with open(sys.argv[3], 'w') as f:
    f.write('functions = [\n' + ''.join('    { address = 0x%X, size = 0x%X },\n' % fn for fn in out) + ']\n')
print('explicit functions: %d   suspicious ends: %d' % (len(out), len(problems)))
for p in problems: print('  ', p)
