"""Load an AmigaDOS hunk executable at fixed addresses (the static recompiler's memory layout)."""
import struct
from dataclasses import dataclass, field
from typing import List, Dict

HUNK_HEADER, HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3f3, 0x3e9, 0x3ea, 0x3eb
HUNK_RELOC32, HUNK_SYMBOL, HUNK_DEBUG, HUNK_END = 0x3ec, 0x3f0, 0x3f1, 0x3f2

# Hunks are placed one after another from LOAD_BASE, each aligned to 8 bytes (AllocMem granularity), with an
# 8-byte segment-list header before each (BPTR to next + size), as LoadSeg does.  The game sees its own hunks at
# these addresses; the generated C bakes them in.
LOAD_BASE = 0x00040000


@dataclass
class Hunk:
    index: int
    kind: str             # CODE DATA BSS
    size: int             # bytes in memory (from the header table)
    mem_flags: int        # 0 any, 1 chip, 2 fast (header table bits 30/31)
    data: bytearray       # initialised bytes (len <= size)
    relocs: Dict[int, List[int]] = field(default_factory=dict)  # target hunk -> offsets of longs in this hunk
    base: int = 0         # load address of the hunk's first byte


@dataclass
class Image:
    hunks: List[Hunk]
    mem: bytearray        # flat image from LOAD_BASE
    start: int            # LOAD_BASE
    reloc_sites: Dict[int, int] = field(default_factory=dict)  # absolute address of a relocated long -> target hunk

    def end(self):
        return self.start + len(self.mem)

    def contains(self, a):
        return self.start <= a < self.end()

    def read16(self, a):
        o = a - self.start
        if o < 0 or o + 2 > len(self.mem):
            raise IndexError('read16 outside image: $%x' % a)
        return (self.mem[o] << 8) | self.mem[o + 1]

    def read32(self, a):
        return (self.read16(a) << 16) | self.read16(a + 2)

    def hunk_at(self, a):
        for h in self.hunks:
            if h.base <= a < h.base + h.size:
                return h
        return None


def load(path, base=LOAD_BASE):
    d = open(path, 'rb').read()
    p = 0

    def u32():
        nonlocal p
        v = struct.unpack_from('>I', d, p)[0]
        p += 4
        return v

    if u32() != HUNK_HEADER:
        raise ValueError('not a hunk executable')
    while True:                      # resident library names (none in practice)
        n = u32()
        if n == 0:
            break
        p += 4 * n
    table_size, first, last = u32(), u32(), u32()
    sizes = [u32() for _ in range(last - first + 1)]
    hunks = []
    cur = None
    while p < len(d):
        t = u32() & 0x3fffffff
        if t in (HUNK_CODE, HUNK_DATA, HUNK_BSS):
            n = u32() * 4
            i = len(hunks)
            flags = sizes[i] >> 30
            size = (sizes[i] & 0x3fffffff) * 4
            kind = {HUNK_CODE: 'CODE', HUNK_DATA: 'DATA', HUNK_BSS: 'BSS'}[t]
            body = bytearray(d[p:p + n]) if t != HUNK_BSS else bytearray()
            if t != HUNK_BSS:
                p += n
            cur = Hunk(i, kind, max(size, n), flags, body)
            hunks.append(cur)
        elif t == HUNK_RELOC32:
            while True:
                c = u32()
                if c == 0:
                    break
                tgt = u32()
                offs = list(struct.unpack_from('>%dI' % c, d, p))
                p += 4 * c
                cur.relocs.setdefault(tgt, []).extend(offs)
        elif t == HUNK_SYMBOL:
            while True:
                c = u32()
                if c == 0:
                    break
                p += 4 * c + 4
        elif t == HUNK_DEBUG:
            p += 4 * u32()
        elif t == HUNK_END:
            cur = None
        else:
            raise ValueError('unsupported hunk type $%x at file offset $%x' % (t, p - 4))

    # place hunks
    a = base
    for h in hunks:
        a += 8                      # segment header (size, next BPTR) like LoadSeg
        h.base = a
        a += (h.size + 7) & ~7
    mem = bytearray(a - base)
    img = Image(hunks, mem, base)
    for h in hunks:
        o = h.base - base
        mem[o:o + len(h.data)] = h.data
        mem[o - 8:o - 4] = struct.pack('>I', h.size + 8)      # AllocMem-style length longword
    for h in hunks:                  # segment list: BPTR (addr/4) of the next segment's link longword
        o = h.base - base
        nxt = hunks[h.index + 1].base - 4 if h.index + 1 < len(hunks) else 0
        mem[o - 4:o] = struct.pack('>I', nxt >> 2)
    for h in hunks:
        for tgt, offs in h.relocs.items():
            add = hunks[tgt].base
            for off in offs:
                at = h.base + off
                v = img.read32(at)
                struct.pack_into('>I', mem, at - base, (v + add) & 0xffffffff)
                img.reloc_sites[at] = tgt
    return img
