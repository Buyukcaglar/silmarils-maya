"""Parse an AmigaDOS hunk executable: list hunks, sizes, relocation counts."""
import struct, sys
NAMES = {0x3e7:'UNIT',0x3e8:'NAME',0x3e9:'CODE',0x3ea:'DATA',0x3eb:'BSS',0x3ec:'RELOC32',0x3ed:'RELOC16',0x3ee:'RELOC8',
         0x3ef:'EXT',0x3f0:'SYMBOL',0x3f1:'DEBUG',0x3f2:'END',0x3f3:'HEADER'}
def parse(path):
    d = open(path, 'rb').read(); p = 0
    u32 = lambda: struct.unpack_from('>I', d, p)[0]
    assert u32() == 0x3f3; p += 4
    while u32(): p += 4 + 4 * u32()
    p += 4
    tab = u32(); first = struct.unpack_from('>I', d, p+4)[0]; last = struct.unpack_from('>I', d, p+8)[0]; p += 12
    sizes = [struct.unpack_from('>I', d, p + 4*i)[0] for i in range(last - first + 1)]; p += 4 * len(sizes)
    hunks = []; cur = None
    while p < len(d):
        t = u32() & 0x3fffffff; p += 4
        n = NAMES.get(t, hex(t))
        if n in ('CODE', 'DATA'):
            ln = u32() * 4; p += 4; cur = {'type': n, 'off': p, 'len': ln, 'relocs': {}}; hunks.append(cur); p += ln
        elif n == 'BSS':
            ln = u32() * 4; p += 4; cur = {'type': n, 'off': None, 'len': ln, 'relocs': {}}; hunks.append(cur)
        elif n == 'RELOC32':
            while True:
                c = u32(); p += 4
                if not c: break
                tgt = u32(); p += 4
                cur['relocs'].setdefault(tgt, []).extend(struct.unpack_from('>%dI' % c, d, p)); p += 4 * c
        elif n == 'END': pass
        elif n in ('SYMBOL',):
            while True:
                c = u32(); p += 4
                if not c: break
                p += 4 * c + 4
        elif n == 'DEBUG':
            p += 4 + u32() * 4
        else:
            raise ValueError('unhandled hunk %s at %x' % (n, p))
    return sizes, hunks, d
if __name__ == '__main__':
    sizes, hunks, d = parse(sys.argv[1])
    print('header sizes (longs, w/ mem flags):', [hex(s) for s in sizes])
    for i, h in enumerate(hunks):
        print(i, h['type'], 'len', hex(h['len']), 'file_off', h['off'] and hex(h['off']),
              'relocs', {k: len(v) for k, v in h['relocs'].items()})
