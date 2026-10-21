"""Find jump tables XenonAnalyse misses and write them in its TOML format.

This compiler emits: lis r12,hi / rlwinm r0,rX,2,0,29 / addi r12,r12,lo / lwzx r0,r12,r0 / mtctr r0 / bctr,
with the absolute table placed right after the bctr.
"""
import bisect, struct, sys

img = open(sys.argv[1], 'rb').read()
BASE, TEXT, TEXT_END = 0x82000000, 0x82140000, 0x828AF6A8  # .text, BINK and RADCODE code sections
w = lambda a: struct.unpack_from('>I', img, a - BASE)[0]
s16 = lambda v: v - 0x10000 if v & 0x8000 else v

pdata_b, pdata_s = 0x82105600, 0x30F50
funcs = []
for o in range(0, pdata_s, 8):
    start, packed = struct.unpack_from('>II', img, pdata_b - BASE + o)
    if start: funcs.append((start, ((packed >> 8) & 0x3FFFFF) * 4))
funcs.sort()
starts = [f[0] for f in funcs]

def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return funcs[i] if i >= 0 and a < funcs[i][0] + funcs[i][1] else None

tables, unmatched = [], []
for a in range(TEXT, TEXT_END, 4):
    if w(a) != 0x7C0903A6 or w(a + 4) != 0x4E800420:  # mtctr r0; bctr
        continue
    pre = [w(a - 4 * k) for k in (4, 3, 2, 1)]
    if pre[3] != 0x7C0C002E:  # lwzx r0,r12,r0
        unmatched.append(a); continue
    hi = lo = rx = lis_at = None
    for k, x in zip((4, 3, 2), pre[:3]):
        if x >> 16 == 0x3D80: hi, lis_at = x & 0xFFFF, a - 4 * k
        elif x >> 16 == 0x398C: lo = s16(x & 0xFFFF)
        elif x & 0xFC1FFFFF == 0x5400103A: rx = (x >> 21) & 31
    if None in (hi, lo, rx) or ((hi << 16) + lo) & 0xFFFFFFFF != a + 8:
        unmatched.append(a); continue

    count = default = None
    for k in range(1, 40):
        x = w(lis_at - 4 * k)
        if x >> 26 == 16 and ((x >> 21) & 31) in (12, 4) and ((x >> 16) & 3) == 1 and default is None:
            cr, default = (x >> 18) & 7, lis_at - 4 * k + s16(x & 0xFFFC)
            for j in range(k + 1, k + 8):
                y = w(lis_at - 4 * j)
                if y >> 26 == 10 and (y >> 23) & 7 == cr and (y >> 16) & 31 == rx:
                    count = (y & 0xFFFF) + 1
                    break
            break
    if count is None:
        # Some tables (eg. in Bink) bound the index with a mask instead: clrlwi rX,rY,MB gives 2^(32-MB) cases.
        for k in range(5, 12):
            x = w(a - 4 * k)
            if x >> 26 == 21 and (x >> 16) & 31 == rx and (x >> 11) & 31 == 0 and (x >> 1) & 31 == 31:
                count = 1 << (32 - ((x >> 6) & 31))
                break
    fn = func_of(a)
    # Frameless functions have no .pdata entry; their cases still land just past the table.
    lo_, hi_ = fn if fn else (a + 8, 0x10000)
    if count is None:  # no bounds check: take entries while they land inside the function
        count = 0
        while lo_ <= w(a + 8 + 4 * count) < lo_ + hi_: count += 1
    labels = [w(a + 8 + 4 * i) for i in range(count)]
    bad = [l for l in labels if not fn or not (fn[0] <= l < fn[0] + fn[1])]
    tables.append((lis_at, rx, default or 0, labels, bad))

# Offset tables: target = base + (table[index] << shift), with byte or halfword entries:
#   lis/addi r12,table ; [rlwinm r0,rI,1 (halfword scale)] ; lbzx|lhzx r0,r12,rX ; [rlwinm r0,r0,shift]
#   lis/addi r12,base ; add r12,r12,r0 ; mtctr r12 ; bctr      (nops and reordering allowed)
offset_tables = 0
for a in range(TEXT, TEXT_END, 4):
    if w(a) != 0x7D8903A6 or w(a + 4) != 0x4E800420 or w(a - 4) != 0x7D8C0214:  # mtctr r12; bctr; after add r12,r12,r0
        continue
    seq = [(a - 4 * k, w(a - 4 * k)) for k in range(12, 1, -1)]
    load_i = next((i for i, (_, x) in enumerate(seq) if x >> 26 == 31 and ((x >> 1) & 0x3FF) in (87, 279)
                   and (x >> 21) & 31 == 0 and (x >> 16) & 31 == 12), None)
    if load_i is None:
        unmatched.append(a); continue
    load = seq[load_i][1]
    size = 1 if ((load >> 1) & 0x3FF) == 87 else 2
    def pair(items):
        hi = next((x & 0xFFFF for _, x in items if x >> 16 == 0x3D80), None)
        lo = next((s16(x & 0xFFFF) for _, x in items if x >> 16 == 0x398C), None)
        return None if hi is None or lo is None else ((hi << 16) + lo) & 0xFFFFFFFF
    table, base = pair(seq[:load_i]), pair(seq[load_i + 1:])
    rx, shift = (load >> 11) & 31, 0
    for _, x in seq[:load_i]:   # rlwinm r0,rI,SH,0,31-SH scales the index for halfword tables
        if x >> 26 == 21 and (x >> 16) & 31 == 0 and rx == 0:
            rx = (x >> 21) & 31
    for _, x in seq[load_i + 1:]:
        if x >> 26 == 21 and (x >> 16) & 31 == 0 and (x >> 21) & 31 == 0:
            shift = (x >> 11) & 31
    if table is None or base is None or rx == 0:
        unmatched.append(a); continue

    # The guard is a "greater than" branch or conditional return on some CR field, fed by a cmplwi.
    # The compared register may be a copy of the index register, so match on the CR field.
    count = None
    for k in range(1, 24):
        x = w(a - 4 * k)
        is_bc = x >> 26 == 16 and ((x >> 21) & 31) in (12, 4) and ((x >> 16) & 3) == 1
        is_bclr = x >> 26 == 19 and ((x >> 1) & 0x3FF) == 16 and ((x >> 21) & 31) == 12 and ((x >> 16) & 3) == 1
        if is_bc or is_bclr:
            cr = (x >> 18) & 7
            for j in range(k + 1, k + 12):
                y = w(a - 4 * j)
                if y >> 26 == 10 and (y >> 23) & 7 == cr:
                    count = (y & 0xFFFF) + 1
                    break
            break
    read = (lambda t: img[t - BASE]) if size == 1 else (lambda t: struct.unpack_from('>H', img, t - BASE)[0])
    fn = func_of(a)
    if count is None:
        # No explicit bound: count entries while their targets stay inside the function. Extra cases are
        # unreachable; too few would turn a real case into undefined behaviour, so this errs long.
        if not fn:
            unmatched.append(a); continue
        count = 0
        def plausible(target):
            return fn[0] <= target < fn[0] + fn[1] and target % 4 == 0
        while count < 256 and plausible(base + (read(table + count * size) << shift)):
            count += 1
        print('inferred %d cases for table at %X' % (count, a))
    labels = [base + (read(table + i * size) << shift) for i in range(count)]
    bad = [l for l in labels if not fn or not (fn[0] <= l < fn[0] + fn[1])]
    tables.append((a, rx, 0, labels, bad))  # keyed at the mtctr, inside the function
    offset_tables += 1
print('offset tables: %d' % offset_tables)

with open(sys.argv[2], 'w') as out:
    out.write('# Generated by tools/find_switches.py\n')
    for base, rx, default, labels, _ in tables:
        out.write('[[switch]]\nbase = 0x%X\nr = %d\ndefault = 0x%X\nlabels = [\n' % (base, rx, default))
        out.write(''.join('    0x%X,\n' % l for l in labels) + ']\n\n')

print('tables: %d  (entries %d)' % (len(tables), sum(len(t[3]) for t in tables)))
print('tables with labels outside their .pdata function: %d' % sum(1 for t in tables if t[4]))
for t in tables:
    if t[4]: print('   base %X bad %s' % (t[0], ' '.join('%X' % l for l in t[4][:4])))
print('mtctr r0;bctr sites not matched: %d  %s' % (len(unmatched), ' '.join('%X' % u for u in unmatched)))
