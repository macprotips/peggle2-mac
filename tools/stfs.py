import struct, sys, collections

f = open(sys.argv[1], 'rb')
def rd(off, n): f.seek(off); return f.read(n)
u32 = lambda b, o=0: struct.unpack_from('>I', b, o)[0]
i24le = lambda b, o: b[o] | b[o+1] << 8 | b[o+2] << 16

hdr = rd(0, 0x1000)
assert hdr[:4] in (b'LIVE', b'PIRS', b'CON ')
header_size = u32(hdr, 0x340)
print('content type %08X  title id %08X' % (u32(hdr, 0x344), u32(hdr, 0x360)))
print('display name:', hdr[0x411:0x411+0x80].decode('utf-16-be').split('\0')[0])
vd = hdr[0x379:0x379+0x24]
read_only = vd[2] & 1
ft_blocks = struct.unpack_from('<H', vd, 3)[0]
ft_start = i24le(vd, 5)
per_table = 1 if read_only else 2

def block_off(idx):
    blk, base = idx, 0xAA
    for _ in range(3):
        blk += ((idx + base) // base) * per_table
        if idx < base: break
        base *= 0xAA
    return ((header_size + 0xFFF) & ~0xFFF) + (blk << 12)

entries = []
for b in range(ft_blocks):
    data = rd(block_off(ft_start + b), 0x1000)
    for o in range(0, 0x1000, 0x40):
        e = data[o:o+0x40]
        nl = e[0x28] & 0x3F
        if nl == 0: continue
        entries.append(dict(name=e[:nl].decode('latin1'), dir=bool(e[0x28] & 0x80),
                            consec=bool(e[0x28] & 0x40), blocks=i24le(e, 0x29),
                            start=i24le(e, 0x2F), parent=struct.unpack_from('>h', e, 0x32)[0],
                            size=u32(e, 0x34)))

def path(i):
    p, e = [], entries[i]
    while True:
        p.append(e['name'])
        if e['parent'] == -1: break
        e = entries[e['parent']]
    return '/'.join(reversed(p))

def read_file(e, n=None):
    # Note: assumes consecutive blocks (normal for read-only XBLA); follow hash-table chain if not
    n = e['size'] if n is None else min(n, e['size'])
    out = b''.join(rd(block_off(e['start'] + k), 0x1000) for k in range((n + 0xFFF) // 0x1000))
    return out[:n]

files = [(path(i), e) for i, e in enumerate(entries) if not e['dir']]
print('\n%d files, %d dirs' % (len(files), len(entries) - len(files)))
ext = collections.Counter()
extsize = collections.Counter()
for p, e in files:
    x = p.rsplit('.', 1)[-1].lower() if '.' in p.split('/')[-1] else '(none)'
    ext[x] += 1; extsize[x] += e['size']
print('\nby extension (count, MB):')
for x, c in ext.most_common():
    print('  %-10s %5d %9.1f' % (x, c, extsize[x] / 1e6))
print('\nlargest files:')
for p, e in sorted(files, key=lambda t: -t[1]['size'])[:25]:
    print('  %10d %s%s' % (e['size'], p, '' if e['consec'] else '  [non-consecutive]'))
print('\ntop-level:')
for i, e in enumerate(entries):
    if e['parent'] == -1: print('  ', e['name'] + ('/' if e['dir'] else ''), e['size'])

# XEX header (unencrypted part only)
for p, e in files:
    if not p.lower().endswith('.xex'): continue
    x = read_file(e, 0x10000)
    print('\n==', p, x[:4], 'size', e['size'])
    if x[:4] != b'XEX2': continue
    print('module flags %08X  pe offset %X' % (u32(x, 4), u32(x, 8)))
    for k in range(u32(x, 0x14)):
        key, val = u32(x, 0x18 + 8*k), u32(x, 0x1C + 8*k)
        if key == 0x000183FF:
            print('original PE name:', x[val+4:val+u32(x, val)].split(b'\0')[0].decode())
        elif key == 0x00040006:
            ei = x[val:val+24]
            print('exec info: media %08X ver %08X base %08X title %08X' % struct.unpack_from('>IIII', ei))
        elif key == 0x000103FF:
            size, strsize, nlib = struct.unpack_from('>III', x, val)
            names = x[val+12:val+12+strsize].split(b'\0')
            names = [n.decode() for n in names if n]
            o = val + 12 + strsize
            print('imports:')
            for _ in range(nlib):
                lsize = u32(x, o)
                ver, vmin = u32(x, o + 28), u32(x, o + 32)
                ni, nrec = struct.unpack_from('>HH', x, o + 36)
                print('  %-14s records %4d  ver %d.%d.%d.%d' % (names[ni], nrec,
                      ver >> 28, (ver >> 24) & 0xF, (ver >> 8) & 0xFFFF, ver & 0xFF))
                o += lsize
        elif key == 0x000002FF:
            n = (u32(x, val) - 4) // 16
            print('resources:', [x[val+4+16*r:val+12+16*r].split(b'\0')[0].decode('latin1') for r in range(n)])
        else:
            print('opt header %08X' % key)
