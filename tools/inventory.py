"""Inventory a dumped XEX image (from xexdump): functions, save/restore helpers, middleware strings."""
import re, struct, sys

img = open(sys.argv[1], 'rb').read()
info = open(sys.argv[2], 'rb').read().decode('latin1')
BASE = 0x82000000
secs = {m[0].split('\x00')[0][:8].strip(): (int(m[1], 16), int(m[2], 16), m[3])
        for m in re.findall(r'section (\S+)\s+([0-9A-F]{8}) ([0-9A-F]{8}) (\w+)', info)}
code = [(n, b, s) for n, (b, s, k) in secs.items() if k == 'code']

pb, ps, _ = secs['.pdata']
pdata = img[pb - BASE:pb - BASE + ps]
funcs = []
for o in range(0, len(pdata), 8):
    start, packed = struct.unpack_from('>II', pdata, o)
    if start == 0: break
    funcs.append((start, ((packed >> 8) & 0x3FFFFF) * 4))
print('.pdata functions: %d, %.1f MB of code covered' % (len(funcs), sum(s for _, s in funcs) / 1e6))
print('code sections:', ', '.join('%s %.2f MB' % (n, s / 1e6) for n, _, s in code))

patterns = {
    'restgprlr_14': 'e9c1ff68', 'savegprlr_14': 'f9c1ff68',
    'restfpr_14': 'c9ccff70', 'savefpr_14': 'd9ccff70',
    'restvmx_14': '3960fee07dcb60ce', 'savevmx_14': '3960fee07dcb61ce',
    'restvmx_64': '3960fc00100b60cb', 'savevmx_64': '3960fc00100b61cb',
}
print('\nsave/restore helpers:')
for name, hexpat in patterns.items():
    pat = bytes.fromhex(hexpat)
    hits = []
    for n, b, s in code:
        seg = img[b - BASE:b - BASE + s]
        hits += ['0x%08X' % (b + m.start()) for m in re.finditer(re.escape(pat), seg) if m.start() % 4 == 0]
    print('  %-14s %s' % (name, ' '.join(hits[:4]) or 'NOT FOUND'))

text_b, text_s = secs['.text'][:2]
text = img[text_b - BASE:text_b - BASE + text_s]
words = struct.unpack('>%dI' % (len(text) // 4), text[:len(text) // 4 * 4])
mtctr = sum(1 for w in words if w == 0x7C0903A6)     # mtctr r0
bctr = sum(1 for w in words if w == 0x4E800420)
print('\nmtctr r0: %d   bctr: %d   (jump table candidates)' % (mtctr, bctr))

print('\nmiddleware / engine strings:')
keys = rb'(?i)(bink[^\x00]{0,40}|rad game tools[^\x00]{0,40}|xaudio2?[^\x00]{0,30}|fmod[^\x00]{0,30}|wwise[^\x00]{0,30}|havok[^\x00]{0,30}|box2d[^\x00]{0,30}|popcap[^\x00]{0,40}|sexy[a-z]*[^\x00]{0,30}|lua[ _]?[0-9][^\x00]{0,20}|xact[^\x00]{0,30}|xna[^\x00]{0,30}|d3d[a-z0-9_]{0,30}|xui[a-z]{0,20}|\.rsb\b|\.rton|\.ptx|\.bik|\.xma|\.xwb|\.xsb|\.ttf|copyright[^\x00]{0,60}|version [0-9][^\x00]{0,30})'
seen = {}
for m in re.finditer(keys, img):
    k = m.group(0).decode('latin1').strip()
    if len(k) >= 4 and k.lower() not in seen: seen[k.lower()] = k
for k in sorted(seen.values(), key=str.lower)[:150]:
    print('  ', k)
