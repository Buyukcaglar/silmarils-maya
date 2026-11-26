"""MC68000 instruction decoder producing structured instructions for analysis and C generation.

Only the 68000 instruction set is decoded (the target is a 1989 OCS Amiga program).
`read16(addr)` supplies big-endian words; decoding never touches anything else.
"""
from dataclasses import dataclass, field
from typing import Optional, List

CC_NAMES = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']
SIZE_SUFFIX = {1: '.b', 2: '.w', 4: '.l', None: ''}


class DecodeError(Exception):
    pass


@dataclass
class EA:
    kind: str                 # dn an ind postinc predec disp index absw absl pcdisp pcindex imm sr ccr usp reglist
    reg: int = 0
    disp: int = 0             # displacement (disp/index/pcdisp/pcindex), absolute address (absw/absl), value (imm)
    xreg: Optional[tuple] = None   # index register: ('d'|'a', n, 2|4)
    ext_at: int = 0           # address of the extension word(s) (lets reloc lookups see absl/imm longs)
    reglist: int = 0
    target: Optional[int] = None   # resolved address for pc-relative modes

    def is_mem(self):
        return self.kind in ('ind', 'postinc', 'predec', 'disp', 'index', 'absw', 'absl', 'pcdisp', 'pcindex')

    def __str__(self):
        k = self.kind
        if k == 'dn': return 'd%d' % self.reg
        if k == 'an': return 'a%d' % self.reg if self.reg < 7 else 'sp'
        if k == 'ind': return '(%s)' % _an(self.reg)
        if k == 'postinc': return '(%s)+' % _an(self.reg)
        if k == 'predec': return '-(%s)' % _an(self.reg)
        if k == 'disp': return '%d(%s)' % (self.disp, _an(self.reg))
        if k == 'index': return '%d(%s,%s)' % (self.disp, _an(self.reg), _xr(self.xreg))
        if k == 'absw': return '$%x.w' % (self.disp & 0xffffffff)
        if k == 'absl': return '$%x' % self.disp
        if k == 'pcdisp': return '$%x(pc)' % self.target
        if k == 'pcindex': return '$%x(pc,%s)' % (self.target, _xr(self.xreg))
        if k == 'imm': return '#$%x' % (self.disp & 0xffffffff)
        if k in ('sr', 'ccr', 'usp'): return k
        if k == 'reglist': return reglist_str(self.reglist)
        return '?'


def _an(r): return 'sp' if r == 7 else 'a%d' % r
def _xr(x): return '%s%d.%s' % (x[0], x[1], 'w' if x[2] == 2 else 'l')


def reglist_str(mask):
    """mask bit i: 0-7 = d0-d7, 8-15 = a0-a7 (normalised, never the predecrement order)."""
    names = ['d%d' % i for i in range(8)] + ['a%d' % i for i in range(8)]
    return '/'.join(n for i, n in enumerate(names) if mask >> i & 1) or '-'


@dataclass
class Insn:
    addr: int
    length: int = 2
    op: str = ''
    size: Optional[int] = None
    src: Optional[EA] = None
    dst: Optional[EA] = None
    cc: Optional[int] = None          # condition code for Bcc/DBcc/Scc
    target: Optional[int] = None      # branch target for Bcc/BRA/BSR/DBcc
    extra: dict = field(default_factory=dict)

    @property
    def end(self):
        return self.addr + self.length

    def __str__(self):
        m = self.op
        if self.cc is not None:
            m = m.replace('cc', CC_NAMES[self.cc])
        m += SIZE_SUFFIX[self.size]
        ops = []
        if self.src is not None: ops.append(str(self.src))
        if self.dst is not None: ops.append(str(self.dst))
        if self.target is not None: ops.append('$%x' % self.target)
        if 'vector' in self.extra: ops.append('#%d' % self.extra['vector'])
        return '%-8s %s' % (m, ','.join(ops))


def sx8(v): return v - 0x100 if v & 0x80 else v
def sx16(v): return v - 0x10000 if v & 0x8000 else v
def sx32(v): return v - 0x100000000 if v & 0x80000000 else v


class Decoder:
    def __init__(self, read16):
        self.read16 = read16

    # ---------------------------------------------------------------- helpers
    def _word(self, st):
        w = self.read16(st['pc'])
        st['pc'] += 2
        return w

    def _ea(self, st, mode, reg, size):
        """Decode an effective address; `size` (1/2/4) only matters for immediates."""
        if mode == 0: return EA('dn', reg)
        if mode == 1: return EA('an', reg)
        if mode == 2: return EA('ind', reg)
        if mode == 3: return EA('postinc', reg)
        if mode == 4: return EA('predec', reg)
        if mode == 5:
            at = st['pc']
            return EA('disp', reg, sx16(self._word(st)), ext_at=at)
        if mode == 6:
            at = st['pc']
            ext = self._word(st)
            if ext & 0x0100:
                raise DecodeError('full extension word (68020+)')
            x = ('a' if ext & 0x8000 else 'd', (ext >> 12) & 7, 4 if ext & 0x0800 else 2)
            return EA('index', reg, sx8(ext & 0xff), x, ext_at=at)
        if mode == 7:
            if reg == 0:
                at = st['pc']
                return EA('absw', disp=sx16(self._word(st)) & 0xffffffff, ext_at=at)
            if reg == 1:
                at = st['pc']
                hi = self._word(st); lo = self._word(st)
                return EA('absl', disp=(hi << 16) | lo, ext_at=at)
            if reg == 2:
                at = st['pc']
                d = sx16(self._word(st))
                return EA('pcdisp', disp=d, ext_at=at, target=(at + d) & 0xffffffff)
            if reg == 3:
                at = st['pc']
                ext = self._word(st)
                if ext & 0x0100:
                    raise DecodeError('full extension word (68020+)')
                x = ('a' if ext & 0x8000 else 'd', (ext >> 12) & 7, 4 if ext & 0x0800 else 2)
                d = sx8(ext & 0xff)
                return EA('pcindex', disp=d, xreg=x, ext_at=at, target=(at + d) & 0xffffffff)
            if reg == 4:
                at = st['pc']
                if size == 4:
                    hi = self._word(st); lo = self._word(st)
                    return EA('imm', disp=(hi << 16) | lo, ext_at=at)
                w = self._word(st)
                return EA('imm', disp=(w & 0xff) if size == 1 else w, ext_at=at)
        raise DecodeError('bad effective address mode %d/%d' % (mode, reg))

    def _imm(self, st, size):
        return self._ea(st, 7, 4, size)

    @staticmethod
    def _size2(bits):
        return {0: 1, 1: 2, 2: 4}.get(bits)

    # ---------------------------------------------------------------- decode
    def decode(self, addr):
        st = {'pc': addr + 2}
        w = self.read16(addr)
        i = Insn(addr)
        g = w >> 12
        mode = (w >> 3) & 7
        reg = w & 7
        rx = (w >> 9) & 7
        getattr(self, '_g%x' % g)(i, w, st, mode, reg, rx)
        i.length = st['pc'] - addr
        return i

    def _bad(self, w):
        raise DecodeError('illegal opcode $%04x' % w)

    def _g0(self, i, w, st, mode, reg, rx):
        if w & 0x0100:
            if mode == 1:   # MOVEP
                opm = (w >> 6) & 7
                i.op = 'movep'; i.size = 2 if opm in (4, 6) else 4
                at = st['pc']
                ea = EA('disp', reg, sx16(self._word(st)), ext_at=at)
                if opm in (4, 5): i.src, i.dst = ea, EA('dn', rx)
                else: i.src, i.dst = EA('dn', rx), ea
                return
            i.op = ['btst', 'bchg', 'bclr', 'bset'][(w >> 6) & 3]
            i.size = 4 if mode == 0 else 1
            i.src = EA('dn', rx)
            i.dst = self._ea(st, mode, reg, 1)
            return
        kind = (w >> 9) & 7
        if kind == 4:   # static bit operation
            i.op = ['btst', 'bchg', 'bclr', 'bset'][(w >> 6) & 3]
            i.size = 4 if mode == 0 else 1
            i.src = self._imm(st, 1)
            i.dst = self._ea(st, mode, reg, 1)
            return
        names = {0: 'ori', 1: 'andi', 2: 'subi', 3: 'addi', 5: 'eori', 6: 'cmpi'}
        if kind not in names: self._bad(w)
        sz = self._size2((w >> 6) & 3)
        if sz is None: self._bad(w)
        i.op = names[kind]
        if (w & 0x3f) == 0x3c and kind in (0, 1, 5):
            if sz == 4: self._bad(w)
            i.size = sz
            i.src = self._imm(st, sz)
            i.dst = EA('ccr' if sz == 1 else 'sr')
            return
        i.size = sz
        i.src = self._imm(st, sz)
        i.dst = self._ea(st, mode, reg, sz)

    def _move(self, i, w, st, mode, reg, rx, sz):
        dmode = (w >> 6) & 7
        i.src = self._ea(st, mode, reg, sz)
        if dmode == 1:
            if sz == 1: self._bad(w)
            i.op = 'movea'
        else:
            i.op = 'move'
        i.size = sz
        i.dst = self._ea(st, dmode, rx, sz)

    def _g1(self, i, w, st, mode, reg, rx): self._move(i, w, st, mode, reg, rx, 1)
    def _g2(self, i, w, st, mode, reg, rx): self._move(i, w, st, mode, reg, rx, 4)
    def _g3(self, i, w, st, mode, reg, rx): self._move(i, w, st, mode, reg, rx, 2)

    def _g4(self, i, w, st, mode, reg, rx):
        if w == 0x4afc: i.op = 'illegal'; return
        if (w & 0xfff0) == 0x4e40: i.op = 'trap'; i.extra['vector'] = w & 15; return
        if (w & 0xfff8) == 0x4e50:
            i.op = 'link'; i.src = EA('an', reg); i.dst = self._imm(st, 2); i.size = 2; return
        if (w & 0xfff8) == 0x4e58: i.op = 'unlk'; i.src = EA('an', reg); return
        if (w & 0xfff8) == 0x4e60: i.op = 'move'; i.size = 4; i.src = EA('an', reg); i.dst = EA('usp'); return
        if (w & 0xfff8) == 0x4e68: i.op = 'move'; i.size = 4; i.src = EA('usp'); i.dst = EA('an', reg); return
        simple = {0x4e70: 'reset', 0x4e71: 'nop', 0x4e73: 'rte', 0x4e75: 'rts', 0x4e76: 'trapv', 0x4e77: 'rtr'}
        if w in simple: i.op = simple[w]; return
        if w == 0x4e72: i.op = 'stop'; i.src = self._imm(st, 2); return
        if (w & 0xffc0) == 0x4e80: i.op = 'jsr'; i.src = self._ea(st, mode, reg, 4); return
        if (w & 0xffc0) == 0x4ec0: i.op = 'jmp'; i.src = self._ea(st, mode, reg, 4); return
        if w & 0x0100:
            if (w & 0x01c0) == 0x01c0: i.op = 'lea'; i.size = 4; i.src = self._ea(st, mode, reg, 4); i.dst = EA('an', rx); return
            if (w & 0x01c0) == 0x0180: i.op = 'chk'; i.size = 2; i.src = self._ea(st, mode, reg, 2); i.dst = EA('dn', rx); return
            self._bad(w)
        sub = (w >> 8) & 0xf
        szb = (w >> 6) & 3
        if sub == 0x0:
            if szb == 3: i.op = 'move'; i.size = 2; i.src = EA('sr'); i.dst = self._ea(st, mode, reg, 2); return
            i.op = 'negx'; i.size = self._size2(szb); i.dst = self._ea(st, mode, reg, i.size); return
        if sub == 0x2:
            if szb == 3: self._bad(w)
            i.op = 'clr'; i.size = self._size2(szb); i.dst = self._ea(st, mode, reg, i.size); return
        if sub == 0x4:
            if szb == 3: i.op = 'move'; i.size = 2; i.src = self._ea(st, mode, reg, 2); i.dst = EA('ccr'); return
            i.op = 'neg'; i.size = self._size2(szb); i.dst = self._ea(st, mode, reg, i.size); return
        if sub == 0x6:
            if szb == 3: i.op = 'move'; i.size = 2; i.src = self._ea(st, mode, reg, 2); i.dst = EA('sr'); return
            i.op = 'not'; i.size = self._size2(szb); i.dst = self._ea(st, mode, reg, i.size); return
        if sub == 0x8:
            if szb == 0: i.op = 'nbcd'; i.size = 1; i.dst = self._ea(st, mode, reg, 1); return
            if szb == 1:
                if mode == 0: i.op = 'swap'; i.size = 4; i.dst = EA('dn', reg); return
                i.op = 'pea'; i.size = 4; i.src = self._ea(st, mode, reg, 4); return
            if mode == 0: i.op = 'ext'; i.size = 2 if szb == 2 else 4; i.dst = EA('dn', reg); return
            i.op = 'movem'; i.size = 2 if szb == 2 else 4
            mask = self._word(st)
            if mode == 4:   # predecrement: bit 0 = a7 ... bit 15 = d0
                mask = int('{:016b}'.format(mask)[::-1], 2)
            i.src = EA('reglist', reglist=mask)
            i.dst = self._ea(st, mode, reg, i.size)
            return
        if sub == 0xa:
            if szb == 3: i.op = 'tas'; i.size = 1; i.dst = self._ea(st, mode, reg, 1); return
            i.op = 'tst'; i.size = self._size2(szb); i.dst = self._ea(st, mode, reg, i.size); return
        if sub == 0xc and szb in (2, 3):
            i.op = 'movem'; i.size = 2 if szb == 2 else 4
            mask = self._word(st)
            i.src = self._ea(st, mode, reg, i.size)
            i.dst = EA('reglist', reglist=mask)
            return
        self._bad(w)

    def _g5(self, i, w, st, mode, reg, rx):
        if (w >> 6) & 3 == 3:
            cc = (w >> 8) & 0xf
            if mode == 1:
                i.op = 'dbcc'; i.cc = cc; i.size = 2; i.src = EA('dn', reg)
                at = st['pc']
                i.target = (at + sx16(self._word(st))) & 0xffffffff
                return
            i.op = 'scc'; i.cc = cc; i.size = 1; i.dst = self._ea(st, mode, reg, 1)
            return
        q = rx or 8
        i.op = 'subq' if w & 0x0100 else 'addq'
        i.size = self._size2((w >> 6) & 3)
        i.src = EA('imm', disp=q)
        i.dst = self._ea(st, mode, reg, i.size)
        if i.dst.kind == 'an' and i.size == 1: self._bad(w)

    def _g6(self, i, w, st, mode, reg, rx):
        cc = (w >> 8) & 0xf
        d = w & 0xff
        base = i.addr + 2
        if d == 0:
            d = sx16(self._word(st)); i.extra['long'] = True
        else:
            d = sx8(d)
        i.target = (base + d) & 0xffffffff
        if cc == 0: i.op = 'bra'
        elif cc == 1: i.op = 'bsr'
        else: i.op = 'bcc'; i.cc = cc

    def _g7(self, i, w, st, mode, reg, rx):
        if w & 0x0100: self._bad(w)
        i.op = 'moveq'; i.size = 4; i.src = EA('imm', disp=sx8(w & 0xff) & 0xffffffff); i.dst = EA('dn', rx)

    def _arith(self, i, w, st, mode, reg, rx, name):
        opm = (w >> 6) & 7
        sz = {0: 1, 1: 2, 2: 4, 4: 1, 5: 2, 6: 4}[opm]
        i.op = name; i.size = sz
        if opm < 4:
            i.src = self._ea(st, mode, reg, sz); i.dst = EA('dn', rx)
        else:
            i.src = EA('dn', rx); i.dst = self._ea(st, mode, reg, sz)

    def _x(self, i, w, rx, reg, name):
        """ADDX/SUBX/ABCD/SBCD/CMPM-style register/memory pair forms."""
        sz = self._size2((w >> 6) & 3) or 1
        i.op = name; i.size = sz
        if w & 8: i.src, i.dst = EA('predec', reg), EA('predec', rx)
        else: i.src, i.dst = EA('dn', reg), EA('dn', rx)

    def _g8(self, i, w, st, mode, reg, rx):
        opm = (w >> 6) & 7
        if opm == 3: i.op = 'divu'; i.size = 2; i.src = self._ea(st, mode, reg, 2); i.dst = EA('dn', rx); return
        if opm == 7: i.op = 'divs'; i.size = 2; i.src = self._ea(st, mode, reg, 2); i.dst = EA('dn', rx); return
        if (w & 0x1f0) == 0x100: self._x(i, w, rx, reg, 'sbcd'); i.size = 1; return
        self._arith(i, w, st, mode, reg, rx, 'or')

    def _addsub(self, i, w, st, mode, reg, rx, name):
        opm = (w >> 6) & 7
        if opm in (3, 7):
            sz = 2 if opm == 3 else 4
            i.op = name + 'a'; i.size = sz; i.src = self._ea(st, mode, reg, sz); i.dst = EA('an', rx); return
        if (w & 0x130) == 0x100:
            self._x(i, w, rx, reg, name + 'x'); return
        self._arith(i, w, st, mode, reg, rx, name)

    def _g9(self, i, w, st, mode, reg, rx): self._addsub(i, w, st, mode, reg, rx, 'sub')
    def _gd(self, i, w, st, mode, reg, rx): self._addsub(i, w, st, mode, reg, rx, 'add')

    def _gb(self, i, w, st, mode, reg, rx):
        opm = (w >> 6) & 7
        if opm in (3, 7):
            sz = 2 if opm == 3 else 4
            i.op = 'cmpa'; i.size = sz; i.src = self._ea(st, mode, reg, sz); i.dst = EA('an', rx); return
        if opm < 3:
            sz = (1, 2, 4)[opm]
            i.op = 'cmp'; i.size = sz; i.src = self._ea(st, mode, reg, sz); i.dst = EA('dn', rx); return
        sz = (1, 2, 4)[opm - 4]
        if mode == 1:
            i.op = 'cmpm'; i.size = sz; i.src = EA('postinc', reg); i.dst = EA('postinc', rx); return
        i.op = 'eor'; i.size = sz; i.src = EA('dn', rx); i.dst = self._ea(st, mode, reg, sz)

    def _gc(self, i, w, st, mode, reg, rx):
        opm = (w >> 6) & 7
        if opm == 3: i.op = 'mulu'; i.size = 2; i.src = self._ea(st, mode, reg, 2); i.dst = EA('dn', rx); return
        if opm == 7: i.op = 'muls'; i.size = 2; i.src = self._ea(st, mode, reg, 2); i.dst = EA('dn', rx); return
        if (w & 0x1f0) == 0x100: self._x(i, w, rx, reg, 'abcd'); i.size = 1; return
        if (w & 0x1f8) == 0x140: i.op = 'exg'; i.size = 4; i.src = EA('dn', rx); i.dst = EA('dn', reg); return
        if (w & 0x1f8) == 0x148: i.op = 'exg'; i.size = 4; i.src = EA('an', rx); i.dst = EA('an', reg); return
        if (w & 0x1f8) == 0x188: i.op = 'exg'; i.size = 4; i.src = EA('dn', rx); i.dst = EA('an', reg); return
        self._arith(i, w, st, mode, reg, rx, 'and')

    def _ge(self, i, w, st, mode, reg, rx):
        left = bool(w & 0x0100)
        if (w >> 6) & 3 == 3:
            if w & 0x0800: self._bad(w)
            t = (w >> 9) & 3
            i.op = ['as', 'ls', 'rox', 'ro'][t] + ('l' if left else 'r')
            i.size = 2
            i.src = EA('imm', disp=1)
            i.dst = self._ea(st, mode, reg, 2)
            return
        t = (w >> 3) & 3
        i.op = ['as', 'ls', 'rox', 'ro'][t] + ('l' if left else 'r')
        i.size = self._size2((w >> 6) & 3)
        if w & 0x20: i.src = EA('dn', rx)
        else: i.src = EA('imm', disp=rx or 8)
        i.dst = EA('dn', reg)

    def _ga(self, i, w, st, mode, reg, rx): i.op = 'linea'; i.extra['opcode'] = w
    def _gf(self, i, w, st, mode, reg, rx): i.op = 'linef'; i.extra['opcode'] = w
