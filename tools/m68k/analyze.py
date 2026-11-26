"""Recursive-descent control-flow analysis of a loaded hunk image.

Finds reachable instructions from the entry point and from code pointers (relocated longs that point into code
hunks), and reports what it cannot resolve: indirect jumps/calls and the instructions they come from.
"""
import sys, os, collections
sys.path.insert(0, os.path.dirname(__file__))
from decoder import Decoder, DecodeError, Insn
from image import load

STOP = {'rts', 'rte', 'rtr', 'illegal', 'linea', 'linef', 'stop', 'reset'}


class Analysis:
    def __init__(self, img):
        self.img = img
        self.dec = Decoder(img.read16)
        self.insns = {}                 # addr -> Insn
        self.owner = {}                 # every byte address covered by an instruction -> insn addr
        self.entries = set()            # function entries (bsr/jsr targets, code pointers)
        self.labels = set()             # branch targets
        self.indirect = []              # (insn) jmp/jsr through register or memory
        self.code_ptrs = {}             # address of pointer -> target (relocated longs into code)
        self.errors = {}                # addr -> message
        self.overlaps = set()

    def is_code(self, a):
        h = self.img.hunk_at(a)
        return h is not None and h.kind == 'CODE' and a < h.base + len(h.data)

    def trace(self, start, why='entry'):
        work = [start]
        while work:
            a = work.pop()
            while True:
                if a in self.insns:
                    break
                if not self.is_code(a) or a & 1:
                    self.errors[a] = 'flow leaves code (%s)' % why
                    break
                if a in self.owner:
                    self.overlaps.add(a)
                    self.errors[a] = 'flow into the middle of the instruction at $%x' % self.owner[a]
                    break
                try:
                    i = self.dec.decode(a)
                except (DecodeError, IndexError) as e:
                    self.errors[a] = str(e)
                    break
                self.insns[a] = i
                for b in range(a, a + i.length):
                    self.owner.setdefault(b, a)
                op = i.op
                # code pointers carried as relocated immediates/absolutes (lea/pea/move #lbl / jsr lbl ...)
                for ea in (i.src, i.dst):
                    if ea is not None and ea.kind in ('absl', 'imm') and ea.ext_at in self.img.reloc_sites:
                        t = ea.disp
                        if self.is_code(t) and op not in ('jsr', 'jmp'):
                            self.code_ptrs[ea.ext_at] = t
                    if ea is not None and ea.kind == 'pcdisp' and op in ('lea', 'pea') and self.is_code(ea.target):
                        self.code_ptrs[ea.ext_at] = ea.target
                # pea of a code address pushes a return address (the ALIS loop's pea $4200a(pc), the sound
                # player's pea $49504(pc) before jmp (a5)): that address is code
                if op == 'pea' and i.src.kind in ('pcdisp', 'absl') and self.is_code(i.src.target if i.src.kind == 'pcdisp' else i.src.disp):
                    t = i.src.target if i.src.kind == 'pcdisp' else i.src.disp
                    self.labels.add(t)
                    work.append(t)
                if op in ('bra',):
                    self.labels.add(i.target); work.append(i.target); break
                if op in ('bcc', 'dbcc'):
                    self.labels.add(i.target); work.append(i.target); a = i.end; continue
                if op == 'bsr':
                    self.entries.add(i.target); work.append(i.target); a = i.end; continue
                if op in ('jsr', 'jmp'):
                    s = i.src
                    t = None
                    if s.kind in ('absl', 'absw'):
                        t = s.disp
                    elif s.kind == 'pcdisp':
                        t = s.target
                    if t is not None and self.is_code(t):
                        (self.entries if op == 'jsr' else self.labels).add(t)
                        work.append(t)
                    else:
                        self.indirect.append(i)
                    if op == 'jmp':
                        break
                    a = i.end
                    continue
                if op in STOP:
                    break
                a = i.end

    def add_jump_table(self, base, count, kind='rel16'):
        """A table of `count` entries; rel16: signed words relative to `base` (the ALIS dispatch tables)."""
        self.tables = getattr(self, 'tables', [])
        self.tables.append((base, count, kind))
        for k in range(count):
            if kind == 'rel16':
                v = self.img.read16(base + 2 * k)
                t = (base + (v - 0x10000 if v & 0x8000 else v)) & 0xffffffff
            else:
                t = self.img.read32(base + 4 * k)
            if t == base:
                continue                     # offset 0: an undefined slot (points at the table itself)
            self.entries.add(t)
            self.trace(t, 'jump table $%x[%d]' % (base, k))

    def run(self, hints=None, pointers=False):
        first = self.img.hunks[0].base
        self.entries.add(first)
        self.trace(first, 'entry')
        for (base, count, kind) in (hints or {}).get('jump_tables', []):
            self.add_jump_table(base, count, kind)
        for t in (hints or {}).get('entries', []):
            self.entries.add(t)
            self.trace(t, 'hint')
        if not pointers:
            return
        # relocated longs anywhere that point into code: candidate code pointers
        changed = True
        while changed:
            changed = False
            cands = {}
            for site, tgt_h in self.img.reloc_sites.items():
                t = self.img.read32(site)
                if self.is_code(t) and site not in self.owner_insn_ext():
                    cands[site] = t
            cands.update(self.code_ptrs)
            for site, t in cands.items():
                if t not in self.insns and t not in self.errors and not (t & 1):
                    self.entries.add(t)
                    self.trace(t, 'code pointer at $%x' % site)
                    changed = True
                self.code_ptrs[site] = t

    def owner_insn_ext(self):
        # relocated longs that are part of decoded instructions are not data pointers
        if not hasattr(self, '_ext_cache') or self._ext_n != len(self.insns):
            s = set()
            for a, i in self.insns.items():
                for b in range(a, a + i.length):
                    s.add(b)
            self._ext_cache, self._ext_n = s, len(self.insns)
        return self._ext_cache

    # ---------------------------------------------------------------- reports
    def coverage(self):
        out = []
        for h in self.img.hunks:
            if h.kind != 'CODE':
                continue
            n = sum(1 for b in range(h.base, h.base + len(h.data)) if b in self.owner)
            out.append((h.index, len(h.data), n))
        return out

    def gaps(self, minlen=4):
        """undecoded runs inside code hunks"""
        res = []
        for h in self.img.hunks:
            if h.kind != 'CODE':
                continue
            a = h.base; end = h.base + len(h.data)
            while a < end:
                if a in self.owner:
                    a += 1; continue
                b = a
                while b < end and b not in self.owner:
                    b += 1
                if b - a >= minlen:
                    res.append((h.index, a, b))
                a = b
        return res


def listing(an, path):
    img = an.img
    with open(path, 'w') as f:
        for h in img.hunks:
            f.write('\n;---- hunk %d %s base $%06x size $%x\n' % (h.index, h.kind, h.base, h.size))
            if h.kind != 'CODE':
                continue
            a = h.base; end = h.base + len(h.data)
            while a < end:
                if a in an.insns:
                    i = an.insns[a]
                    tag = ''
                    if a in an.entries: f.write('\nsub_%06x:\n' % a)
                    elif a in an.labels: f.write('lbl_%06x:\n' % a)
                    words = ' '.join('%04x' % img.read16(x) for x in range(a, a + i.length, 2))
                    f.write('  %06x  %-30s %s\n' % (a, words, i))
                    a += i.length
                else:
                    b = a
                    while b < end and b not in an.insns:
                        b += 2
                    for x in range(a, b, 16):
                        chunk = img.mem[x - img.start:min(b, x + 16) - img.start]
                        f.write('  %06x  dc.b %s  ; %s\n' % (x, chunk.hex(' '), ''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)))
                    a = b


if __name__ == '__main__':
    img = load(sys.argv[1])
    an = Analysis(img)
    an.run()
    for h in img.hunks:
        print('hunk %d %-4s base $%06x size $%05x init $%05x flags %d' % (h.index, h.kind, h.base, h.size, len(h.data), h.mem_flags))
    print('instructions', len(an.insns), 'entries', len(an.entries), 'labels', len(an.labels))
    for idx, tot, cov in an.coverage():
        print('  code hunk %d: %d/%d bytes decoded (%.1f%%)' % (idx, cov, tot, 100.0 * cov / tot))
    print('errors', len(an.errors))
    for a, e in sorted(an.errors.items())[:40]:
        print('  $%06x %s' % (a, e))
    ops = collections.Counter(i.op for i in an.insns.values())
    print('opcodes', dict(ops.most_common()))
    print('indirect jumps/calls', len(an.indirect))
    kinds = collections.Counter('%s %s' % (i.op, i.src.kind + (('(a%d)' % i.src.reg) if i.src.kind in ('disp', 'ind', 'index') else '')) for i in an.indirect)
    print('  ', dict(kinds))
    if len(sys.argv) > 2:
        listing(an, sys.argv[2])
        print('listing ->', sys.argv[2])
