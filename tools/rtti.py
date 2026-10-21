"""Map vtables to C++ class names using MSVC RTTI in a dumped image.

TypeDescriptor:          pVFTable, spare, ".?AV<name>@@"
CompleteObjectLocator:   signature, offset, cdOffset, pTypeDescriptor, pClassHierarchyDescriptor
A vtable is preceded by a pointer to its CompleteObjectLocator.

usage: rtti.py image.bin [name-filter]   -> prints vtable address, class, offset, method addresses
"""
import re, struct, sys

img = open(sys.argv[1], 'rb').read()
filt = sys.argv[2] if len(sys.argv) > 2 else None
BASE, TEXT, TEXT_END = 0x82000000, 0x82140000, 0x828AF6A8
w = lambda a: struct.unpack_from('>I', img, a - BASE)[0]
words = struct.unpack('>%dI' % (len(img) // 4), img[:len(img) // 4 * 4])

def demangle(n):
    # .?AVFoo@Bar@@ -> Bar::Foo (enough for orientation; templates stay mangled)
    parts = n[4:].rstrip('@').split('@')
    return '::'.join(reversed([p for p in parts if p]))

tds = {}
for m in re.finditer(rb'\.\?A[VU][^\x00]{1,200}\x00', img):
    tds[BASE + m.start() - 8] = demangle(m.group(0)[:-1].decode('latin1'))

cols = {}
for i in range(len(words) - 4):
    if words[i] == 0 and words[i + 3] in tds:
        cols[BASE + i * 4] = (tds[words[i + 3]], words[i + 1])

vtables = []
for i in range(1, len(words)):
    col = words[i - 1]
    if col in cols and TEXT <= words[i] < TEXT_END:
        methods = []
        j = i
        while j < len(words) and TEXT <= words[j] < TEXT_END:
            methods.append(words[j]); j += 1
        vtables.append((BASE + i * 4, cols[col][0], cols[col][1], methods))

print('# type descriptors: %d  object locators: %d  vtables: %d  methods: %d' %
      (len(tds), len(cols), len(vtables), sum(len(v[3]) for v in vtables)))
for addr, name, off, methods in vtables:
    if filt and filt.lower() not in name.lower(): continue
    print('%08X %s (offset %d): %d methods  %s' % (addr, name, off, len(methods), ' '.join('%X' % m for m in methods[:6])))
