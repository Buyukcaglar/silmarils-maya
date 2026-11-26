"""Static recompiler: T.X (Amiga hunk executable, MC68000) to C.

    python -I tools/m68k/gen_c.py <path to T.X> <output directory>

Writes tx_code.c (the program as C), tx_image.c (the loaded, relocated and patched memory image) and tx_gen.h.
The output is generated from the user's own copy of the game at build time and is never committed.

Model (docs/RECOMP-PLAN.md): the 68000 state lives in the global `M` (runtime/cpu.h) and the stack in emulated
memory.  The code is split into regions (one per entry point: subroutine, table handler, interrupt handler); a
region is one C function whose instructions are labels.  A branch inside the region is a goto; every other transfer
(calls, returns, indirect jumps, branches to other regions) sets M.pc and returns to the dispatcher, which enters
the region holding M.pc through the switch at its top.  Backward gotos check M.ev, so hardware events and
interrupts are taken at loop heads, region exits and returns.  An instruction that touches the hardware (a CIA or
custom chip register) checks M.ev first as well: an interrupt that is due is then taken before the access, as on
the 68000, and not after it (docs/DECISIONS.md D32: the music stop's "andi.b #$f6,$bfde00" lost a race with a due
CIA-B timer interrupt, whose handler then restarted the timer the game had just stopped).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from image import load                                  # noqa: E402
from decoder import Decoder, DecodeError, CC_NAMES      # noqa: E402
from analyze import Analysis                            # noqa: E402
import hints_maya                                       # noqa: E402
import patches_maya                                     # noqa: E402

HW_LO = 0xbf0000          # CIAs at $bfd000/$bfe001, custom chips at $dff000


def _hw_const(ea):
    """The hardware address an operand names outright (absolute, or an immediate loaded as an address)."""
    if ea is None:
        return False
    if ea.kind in ('absw', 'absl'):
        return HW_LO <= (ea.disp & 0xffffff) and (ea.disp & 0xff000000) == 0
    return False


def hw_base_regs(insns):
    """The address registers a region ever loads with a hardware address (lea $dff000,a0; movea.l #$dff0a0,a6)."""
    regs = set()
    for i in insns:
        if i.dst is None or i.dst.kind != 'an' or i.src is None:
            continue
        if i.op == 'lea' and _hw_const(i.src):
            regs.add(i.dst.reg)
        elif i.op in ('move', 'movea') and i.src.kind == 'imm' and HW_LO <= (i.src.disp & 0xffffffff) <= 0xffffff:
            regs.add(i.dst.reg)
    return regs


def touches_hw(i, regs):
    """1 when the instruction reads or writes a hardware register, or may (through a register the region uses as a
    hardware base).  Over-approximate on purpose: an extra check costs one compare."""
    if i.op == 'lea':
        return False
    for ea in (i.src, i.dst):
        if ea is None:
            continue
        if _hw_const(ea):
            return True
        if regs and ea.kind in ('ind', 'postinc', 'predec', 'disp', 'index') and ea.reg in regs:
            return True
    return False


MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
MASKV = {1: 0xff, 2: 0xffff, 4: 0xffffffff}
RD = {1: 'RB', 2: 'RW', 4: 'RL'}
WR = {1: 'WB', 2: 'WW', 4: 'WL'}
CC = ['1', '0', '(!M.c && !M.z)', '(M.c || M.z)', '(!M.c)', '(M.c)', '(!M.z)', '(M.z)', '(!M.v)', '(M.v)',
      '(!M.n)', '(M.n)', '(M.n == M.v)', '(M.n != M.v)', '(!M.z && M.n == M.v)', '(M.z || M.n != M.v)']
EA_T = {'dn': (0, 0), 'an': (0, 0), 'ind': (4, 8), 'postinc': (4, 8), 'predec': (6, 10), 'disp': (8, 12),
        'index': (10, 14), 'absw': (8, 12), 'absl': (12, 16), 'pcdisp': (8, 12), 'pcindex': (10, 14),
        'imm': (4, 8), 'sr': (0, 0), 'ccr': (0, 0), 'usp': (0, 0), 'reglist': (0, 0)}
FLOW_END = {'bra', 'jmp', 'rts', 'rte', 'rtr', 'illegal', 'linea', 'linef', 'stop', 'reset', 'trap'}


class GenError(Exception):
    pass


def ea_time(ea, sz):
    if ea is None:
        return 0
    t = EA_T[ea.kind]
    return t[1] if sz == 4 else t[0]


def cycles(i):
    """A plain estimate of the instruction's 68000 clock cycles (it paces the emulated hardware clock)."""
    op, sz, s, d = i.op, i.size or 2, i.src, i.dst
    mem_d = d is not None and d.is_mem()
    if op in ('move', 'movea'):
        return 4 + ea_time(s, sz) + ea_time(d, sz)
    if op == 'moveq':
        return 4
    if op == 'lea':
        return max(4, ea_time(s, 4) - 4)
    if op == 'pea':
        return 8 + ea_time(s, 4)
    if op in ('add', 'sub', 'and', 'or', 'eor', 'cmp', 'adda', 'suba', 'cmpa'):
        return (8 + ea_time(d, sz)) if mem_d else (6 if sz == 4 else 4) + ea_time(s, sz)
    if op in ('addq', 'subq'):
        return (8 + ea_time(d, sz)) if mem_d else (8 if sz == 4 or d.kind == 'an' else 4)
    if op in ('addi', 'subi', 'andi', 'ori', 'eori', 'cmpi'):
        return (12 + ea_time(d, sz)) if mem_d else (16 if sz == 4 else 8)
    if op in ('clr', 'neg', 'negx', 'not', 'scc', 'tas'):
        return (8 + ea_time(d, sz)) if mem_d else (6 if sz == 4 else 4)
    if op == 'tst':
        return 4 + ea_time(d, sz)
    if op in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'):
        if mem_d:
            return 8 + ea_time(d, 2)
        n = s.disp if s.kind == 'imm' else 4
        return (8 if sz == 4 else 6) + 2 * n
    if op in ('btst', 'bset', 'bclr', 'bchg'):
        return (8 if s.kind == 'imm' else 4) + (4 if op != 'btst' else 0) + (ea_time(d, 1) if mem_d else 2)
    if op in ('mulu', 'muls'):
        return 70 + ea_time(s, 2)
    if op == 'divu':
        return 140 + ea_time(s, 2)
    if op == 'divs':
        return 158 + ea_time(s, 2)
    if op == 'movem':
        n = bin((s if s.kind == 'reglist' else d).reglist).count('1')
        ea = d if s.kind == 'reglist' else s
        return 8 + (8 if sz == 4 else 4) * n + ea_time(ea, 2)
    if op in ('bcc',):
        return 10
    if op == 'bra':
        return 10
    if op == 'bsr':
        return 18
    if op == 'dbcc':
        return 10
    if op == 'jsr':
        return 16 + ea_time(s, 2)
    if op == 'jmp':
        return 8 + ea_time(s, 2)
    if op == 'rts':
        return 16
    if op in ('rte', 'rtr'):
        return 20
    if op in ('exg', 'ext', 'swap', 'nop'):
        return 6 if op == 'exg' else 4
    if op == 'link':
        return 16
    if op == 'unlk':
        return 12
    return 8


class Loc:
    """An operand: a register, an immediate or a memory location (its address computed once, side effects done)."""

    def __init__(self, e, ea, sz):
        self.e, self.ea, self.sz = e, ea, sz
        self.addr = e.address(ea, sz) if ea.is_mem() else None

    def get(self):
        k, sz, r = self.ea.kind, self.sz, self.ea.reg
        if k == 'dn':
            return 'M.d[%d]' % r if sz == 4 else '(M.d[%d] & %s)' % (r, MASK[sz])
        if k == 'an':
            return 'M.a[%d]' % r if sz == 4 else '(M.a[%d] & %s)' % (r, MASK[sz])
        if k == 'imm':
            return '0x%xu' % (self.ea.disp & MASKV[sz])
        if self.addr is not None:
            v = self.e.tmp('v')
            self.e.s('uint32_t %s = %s(%s);' % (v, RD[sz], self.addr))
            return v
        raise GenError('cannot read operand %s' % self.ea)

    def set(self, val):
        k, sz, r = self.ea.kind, self.sz, self.ea.reg
        if k == 'dn':
            if sz == 4:
                self.e.s('M.d[%d] = %s;' % (r, val))
            else:
                self.e.s('M.d[%d] = (M.d[%d] & ~%s) | ((%s) & %s);' % (r, r, MASK[sz], val, MASK[sz]))
            return
        if k == 'an':
            self.e.s('M.a[%d] = %s;' % (r, val))
            return
        if self.addr is not None:
            self.e.s('%s(%s, %s);' % (WR[sz], self.addr, val))
            return
        raise GenError('cannot write operand %s' % self.ea)


class Emit:
    """The C statements of one instruction."""

    def __init__(self, gen, i, region):
        self.g, self.i, self.region = gen, i, region
        self.out = []
        self.n = 0

    def tmp(self, p='t'):
        self.n += 1
        return '%s%d' % (p, self.n)

    def s(self, line):
        self.out.append(line)

    @staticmethod
    def xidx(x):
        reg = 'M.%s[%d]' % ('d' if x[0] == 'd' else 'a', x[1])
        return reg if x[2] == 4 else '(uint32_t)(int16_t)%s' % reg

    def address(self, ea, sz):
        k, r = ea.kind, ea.reg
        inc = 2 if (r == 7 and sz == 1) else sz
        if k in ('absw', 'absl'):
            return '0x%xu' % (ea.disp & 0xffffffff)
        if k == 'pcdisp':
            return '0x%xu' % ea.target
        a = self.tmp('a')
        if k == 'ind':
            self.s('uint32_t %s = M.a[%d];' % (a, r))
        elif k == 'postinc':
            self.s('uint32_t %s = M.a[%d]; M.a[%d] += %d;' % (a, r, r, inc))
        elif k == 'predec':
            self.s('M.a[%d] -= %d; uint32_t %s = M.a[%d];' % (r, inc, a, r))
        elif k == 'disp':
            self.s('uint32_t %s = M.a[%d] + (uint32_t)(%d);' % (a, r, ea.disp))
        elif k == 'index':
            self.s('uint32_t %s = M.a[%d] + (uint32_t)(%d) + %s;' % (a, r, ea.disp, self.xidx(ea.xreg)))
        elif k == 'pcindex':
            self.s('uint32_t %s = 0x%xu + %s;' % (a, ea.target, self.xidx(ea.xreg)))
        else:
            raise GenError('no address for %s' % ea)
        return a

    # ------------------------------------------------------------ control transfer
    def branch(self, t):
        """Statements that continue at address t."""
        if t in self.region.addrs:
            if t <= self.i.addr:
                return 'if (M.cyc >= M.ev) { M.pc = 0x%xu; return; } goto L_%06x;' % (t, t)
            return 'goto L_%06x;' % t
        return 'M.pc = 0x%xu; return;' % t

    # ------------------------------------------------------------ instructions
    def emit(self):
        i = self.i
        op = i.op
        f = getattr(self, 'op_' + op, None)
        if f is None:
            if op in ('add', 'sub', 'addi', 'subi', 'addq', 'subq'):
                f = self.op_addsub
            elif op in ('and', 'or', 'eor', 'andi', 'ori', 'eori'):
                f = self.op_logic
            elif op in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'):
                f = self.op_shift
            elif op in ('btst', 'bset', 'bclr', 'bchg'):
                f = self.op_bit
            elif op in ('addx', 'subx'):
                f = self.op_x
            elif op in ('abcd', 'sbcd'):
                f = self.op_bcd
            elif op in ('illegal', 'linea', 'linef', 'reset', 'stop', 'trap'):
                f = self.op_exception
            else:
                raise GenError('$%06x: no C for %s' % (i.addr, i))
        f()
        return self.out

    def op_exception(self):
        vec = {'illegal': 4, 'linea': 10, 'linef': 11, 'reset': 0, 'stop': 0}.get(self.i.op)
        if self.i.op == 'trap':
            vec = 32 + self.i.extra['vector']
        self.s('M.pc = 0x%xu; rt_exception(%d); return;' % (self.i.addr, vec))

    def op_nop(self):
        pass

    def op_move(self):
        i = self.i
        sz = i.size
        s, d = i.src, i.dst
        if s.kind == 'sr':
            Loc(self, d, 2).set('GET_SR()')
            return
        if s.kind == 'usp':
            self.s('M.a[%d] = M.osp;' % d.reg)
            return
        if d.kind == 'usp':
            self.s('M.osp = M.a[%d];' % s.reg)
            return
        v = Loc(self, s, sz).get()
        if d.kind == 'sr':
            self.s('SET_SR(%s);' % v)
            return
        if d.kind == 'ccr':
            self.s('SET_CCR(%s);' % v)
            return
        if s.is_mem() or s.kind in ('dn', 'an'):
            t = self.tmp()
            self.s('uint32_t %s = %s;' % (t, v))
            v = t
        Loc(self, d, sz).set(v)
        self.s('FLG_LOGIC(%s, %d);' % (v, sz))

    def op_moveq(self):
        v = self.i.src.disp & 0xffffffff
        self.s('M.d[%d] = 0x%xu; FLG_LOGIC(0x%xu, 4);' % (self.i.dst.reg, v, v))

    def op_movea(self):
        i = self.i
        v = Loc(self, i.src, i.size).get()
        if i.size == 2:
            v = '(uint32_t)(int16_t)%s' % v
        self.s('M.a[%d] = %s;' % (i.dst.reg, v))

    def op_lea(self):
        a = self.address(self.i.src, 4)
        self.s('M.a[%d] = %s;' % (self.i.dst.reg, a))

    def op_pea(self):
        a = self.address(self.i.src, 4)
        t = self.tmp()
        self.s('uint32_t %s = %s; PUSH32(%s);' % (t, a, t))

    def op_clr(self):
        Loc(self, self.i.dst, self.i.size).set('0')
        self.s('M.n = 0; M.z = 1; M.v = 0; M.c = 0;')

    def op_tst(self):
        v = Loc(self, self.i.dst, self.i.size).get()
        self.s('FLG_LOGIC(%s, %d);' % (v, self.i.size))

    def op_tas(self):
        L = Loc(self, self.i.dst, 1)
        v = L.get()
        t = self.tmp()
        self.s('uint32_t %s = %s; FLG_LOGIC(%s, 1);' % (t, v, t))
        L.set('%s | 0x80u' % t)

    def op_not(self):
        sz = self.i.size
        L = Loc(self, self.i.dst, sz)
        v = L.get()
        t = self.tmp()
        self.s('uint32_t %s = ~%s & %s;' % (t, v, MASK[sz]))
        L.set(t)
        self.s('FLG_LOGIC(%s, %d);' % (t, sz))

    def op_neg(self):
        sz = self.i.size
        L = Loc(self, self.i.dst, sz)
        v = L.get()
        t = self.tmp()
        self.s('uint32_t %s = SUB(%s, 0, %d);' % (t, v, sz))
        L.set(t)

    def op_negx(self):
        sz = self.i.size
        L = Loc(self, self.i.dst, sz)
        v = L.get()
        t = self.tmp()
        self.s('uint32_t %s = SUBX(%s, 0, %d);' % (t, v, sz))
        L.set(t)

    def op_addsub(self):
        i = self.i
        sz = i.size
        add = i.op.startswith('add')
        if i.op in ('addq', 'subq') and i.dst.kind == 'an':
            self.s('M.a[%d] %s= %du;' % (i.dst.reg, '+' if add else '-', i.src.disp))
            return
        sv = Loc(self, i.src, sz).get()
        if i.src.is_mem():
            pass
        D = Loc(self, i.dst, sz)
        dv = D.get()
        t = self.tmp()
        self.s('uint32_t %s = %s(%s, %s, %d);' % (t, 'ADD' if add else 'SUB', sv, dv, sz))
        D.set(t)

    def op_adda(self):
        self._adda('+')

    def op_suba(self):
        self._adda('-')

    def _adda(self, sign):
        i = self.i
        v = Loc(self, i.src, i.size).get()
        if i.size == 2:
            v = '(uint32_t)(int16_t)%s' % v
        self.s('M.a[%d] %s= %s;' % (i.dst.reg, sign, v))

    def op_x(self):
        i = self.i
        sz = i.size
        sv = Loc(self, i.src, sz).get()
        t0 = self.tmp()
        self.s('uint32_t %s = %s;' % (t0, sv))
        D = Loc(self, i.dst, sz)
        dv = D.get()
        t = self.tmp()
        self.s('uint32_t %s = %s(%s, %s, %d);' % (t, 'ADDX' if i.op == 'addx' else 'SUBX', t0, dv, sz))
        D.set(t)

    def op_bcd(self):
        i = self.i
        sv = Loc(self, i.src, 1).get()
        t0 = self.tmp()
        self.s('uint32_t %s = %s;' % (t0, sv))
        D = Loc(self, i.dst, 1)
        dv = D.get()
        t = self.tmp()
        self.s('uint32_t %s = %s(%s, %s);' % (t, i.op.upper(), t0, dv))
        D.set(t)

    def op_nbcd(self):
        D = Loc(self, self.i.dst, 1)
        dv = D.get()
        t = self.tmp()
        self.s('uint32_t %s = SBCD(%s, 0);' % (t, dv))
        D.set(t)

    def op_cmp(self):
        i = self.i
        sv = Loc(self, i.src, i.size).get()
        dv = Loc(self, i.dst, i.size).get()
        self.s('CMP(%s, %s, %d);' % (sv, dv, i.size))

    op_cmpi = op_cmp

    def op_cmpm(self):
        i = self.i
        sv = Loc(self, i.src, i.size).get()
        t0 = self.tmp()
        self.s('uint32_t %s = %s;' % (t0, sv))
        dv = Loc(self, i.dst, i.size).get()
        self.s('CMP(%s, %s, %d);' % (t0, dv, i.size))

    def op_cmpa(self):
        i = self.i
        v = Loc(self, i.src, i.size).get()
        if i.size == 2:
            v = '(uint32_t)(int16_t)%s' % v
        self.s('CMP(%s, M.a[%d], 4);' % (v, i.dst.reg))

    def op_logic(self):
        i = self.i
        sz = i.size
        cop = {'and': '&', 'or': '|', 'eor': '^'}[i.op.rstrip('i')]
        if i.dst.kind in ('ccr', 'sr'):
            imm = i.src.disp & MASKV[sz]
            if i.dst.kind == 'ccr':
                self.s('SET_CCR(GET_CCR() %s 0x%xu);' % (cop, imm))
            else:
                self.s('SET_SR(GET_SR() %s 0x%xu);' % (cop, imm))
            return
        sv = Loc(self, i.src, sz).get()
        t0 = self.tmp()
        self.s('uint32_t %s = %s;' % (t0, sv))
        D = Loc(self, i.dst, sz)
        dv = D.get()
        t = self.tmp()
        self.s('uint32_t %s = (%s %s %s) & %s;' % (t, dv, cop, t0, MASK[sz]))
        D.set(t)
        self.s('FLG_LOGIC(%s, %d);' % (t, sz))

    def op_mulu(self):
        i = self.i
        v = Loc(self, i.src, 2).get()
        r = i.dst.reg
        self.s('M.d[%d] = (M.d[%d] & 0xffffu) * (%s); FLG_LOGIC(M.d[%d], 4);' % (r, r, v, r))

    def op_muls(self):
        i = self.i
        v = Loc(self, i.src, 2).get()
        r = i.dst.reg
        self.s('M.d[%d] = (uint32_t)((int32_t)(int16_t)M.d[%d] * (int32_t)(int16_t)(%s)); FLG_LOGIC(M.d[%d], 4);'
               % (r, r, v, r))

    def op_divu(self):
        i = self.i
        v = Loc(self, i.src, 2).get()
        self.s('if (!DIVU(%d, %s)) { M.pc = 0x%xu; rt_exception(5); return; }' % (i.dst.reg, v, i.addr))

    def op_divs(self):
        i = self.i
        v = Loc(self, i.src, 2).get()
        self.s('if (!DIVS(%d, %s)) { M.pc = 0x%xu; rt_exception(5); return; }' % (i.dst.reg, v, i.addr))

    def op_chk(self):
        i = self.i
        v = Loc(self, i.src, 2).get()
        self.s('if (!CHK(%d, %s)) { M.pc = 0x%xu; rt_exception(6); return; }' % (i.dst.reg, v, i.addr))

    def op_ext(self):
        r = self.i.dst.reg
        if self.i.size == 2:
            self.s('M.d[%d] = (M.d[%d] & 0xffff0000u) | ((uint32_t)(int16_t)(int8_t)M.d[%d] & 0xffffu); '
                   'FLG_LOGIC(M.d[%d], 2);' % (r, r, r, r))
        else:
            self.s('M.d[%d] = (uint32_t)(int32_t)(int16_t)M.d[%d]; FLG_LOGIC(M.d[%d], 4);' % (r, r, r))

    def op_swap(self):
        r = self.i.dst.reg
        self.s('M.d[%d] = (M.d[%d] >> 16) | (M.d[%d] << 16); FLG_LOGIC(M.d[%d], 4);' % (r, r, r, r))

    def op_exg(self):
        s, d = self.i.src, self.i.dst
        a = 'M.%s[%d]' % ('d' if s.kind == 'dn' else 'a', s.reg)
        b = 'M.%s[%d]' % ('d' if d.kind == 'dn' else 'a', d.reg)
        t = self.tmp()
        self.s('uint32_t %s = %s; %s = %s; %s = %s;' % (t, a, a, b, b, t))

    def op_shift(self):
        i = self.i
        fn = i.op.upper()
        if i.dst.is_mem():
            L = Loc(self, i.dst, 2)
            v = L.get()
            t = self.tmp()
            self.s('uint32_t %s = %s(%s, 1, 2);' % (t, fn, v))
            L.set(t)
            return
        sz = i.size
        cnt = '%du' % i.src.disp if i.src.kind == 'imm' else '(M.d[%d] & 63u)' % i.src.reg
        L = Loc(self, i.dst, sz)
        v = L.get()
        t = self.tmp()
        self.s('uint32_t %s = %s(%s, %s, %d);' % (t, fn, v, cnt, sz))
        L.set(t)

    def op_bit(self):
        i = self.i
        reg_dst = i.dst.kind == 'dn'
        m = 31 if reg_dst else 7
        if i.src.kind == 'imm':
            b = '%du' % (i.src.disp & m)
        else:
            b = '(M.d[%d] & %du)' % (i.src.reg, m)
        sz = 4 if reg_dst else 1
        L = Loc(self, i.dst, sz)
        v = L.get()
        t = self.tmp()
        self.s('uint32_t %s = %s; uint32_t %sm = 1u << %s; M.z = (%s & %sm) == 0;' % (t, v, t, b, t, t))
        if i.op == 'bset':
            L.set('%s | %sm' % (t, t))
        elif i.op == 'bclr':
            L.set('%s & ~%sm' % (t, t))
        elif i.op == 'bchg':
            L.set('%s ^ %sm' % (t, t))

    def op_scc(self):
        Loc(self, self.i.dst, 1).set('(%s ? 0xffu : 0u)' % CC[self.i.cc])

    def op_movem(self):
        i = self.i
        sz = i.size
        names = ['M.d[%d]' % k for k in range(8)] + ['M.a[%d]' % k for k in range(8)]
        if i.dst.kind == 'reglist':
            regs = [k for k in range(16) if i.dst.reglist >> k & 1]
            ea = i.src
            a = self.tmp('a')
            if ea.kind == 'postinc':
                self.s('uint32_t %s = M.a[%d];' % (a, ea.reg))
            else:
                self.s('uint32_t %s = %s;' % (a, self.address(ea, sz)))
            for k in regs:
                if sz == 2:
                    self.s('%s = (uint32_t)(int16_t)RW(%s); %s += 2;' % (names[k], a, a))
                else:
                    self.s('%s = RL(%s); %s += 4;' % (names[k], a, a))
            if ea.kind == 'postinc':
                self.s('M.a[%d] = %s;' % (ea.reg, a))
        else:
            regs = [k for k in range(16) if i.src.reglist >> k & 1]
            ea = i.dst
            a = self.tmp('a')
            if ea.kind == 'predec':
                self.s('uint32_t %s = M.a[%d];' % (a, ea.reg))
                for k in reversed(regs):
                    self.s('%s -= %d; %s(%s, %s);' % (a, sz, WR[sz], a, names[k]))
                self.s('M.a[%d] = %s;' % (ea.reg, a))
            else:
                self.s('uint32_t %s = %s;' % (a, self.address(ea, sz)))
                for k in regs:
                    self.s('%s(%s, %s); %s += %d;' % (WR[sz], a, names[k], a, sz))

    def op_movep(self):
        i = self.i
        n = i.size
        if i.src.kind == 'dn':
            a = self.address(i.dst, 1)
            r = i.src.reg
            for k in range(n):
                self.s('WB(%s + %d, M.d[%d] >> %d);' % (a, 2 * k, r, 8 * (n - 1 - k)))
        else:
            a = self.address(i.src, 1)
            r = i.dst.reg
            parts = ' | '.join('(RB(%s + %d) << %d)' % (a, 2 * k, 8 * (n - 1 - k)) for k in range(n))
            if n == 2:
                self.s('M.d[%d] = (M.d[%d] & 0xffff0000u) | %s;' % (r, r, parts))
            else:
                self.s('M.d[%d] = %s;' % (r, parts))

    def op_link(self):
        i = self.i
        r = i.src.reg
        self.s('PUSH32(M.a[%d]); M.a[%d] = M.a[7]; M.a[7] += (uint32_t)(%d);'
               % (r, r, i.dst.disp - 0x10000 if i.dst.disp & 0x8000 else i.dst.disp))

    def op_unlk(self):
        r = self.i.src.reg
        self.s('M.a[7] = M.a[%d]; M.a[%d] = POP32();' % (r, r))

    def op_trapv(self):
        self.s('if (M.v) { M.pc = 0x%xu; rt_exception(7); return; }' % self.i.end)

    # flow
    def op_bra(self):
        self.s(self.branch(self.i.target))

    def op_bcc(self):
        self.s('if %s { %s }' % (CC[self.i.cc], self.branch(self.i.target)))

    def op_dbcc(self):
        i = self.i
        r = i.src.reg
        self.s('if (!%s) { uint32_t c = (M.d[%d] - 1u) & 0xffffu; M.d[%d] = (M.d[%d] & 0xffff0000u) | c; '
               'if (c != 0xffffu) { %s } }' % (CC[i.cc], r, r, r, self.branch(i.target)))

    def op_bsr(self):
        i = self.i
        self.s('PUSH32(0x%xu); %s' % (i.end, self.branch(i.target)))

    def _target(self):
        s = self.i.src
        if s.kind in ('absw', 'absl'):
            return s.disp & 0xffffffff, None
        if s.kind == 'pcdisp':
            return s.target, None
        return None, self.address(s, 4)

    def op_jsr(self):
        t, expr = self._target()
        if t is not None:
            self.s('PUSH32(0x%xu); %s' % (self.i.end, self.branch(t)))
        else:
            v = self.tmp()
            self.s('uint32_t %s = %s; PUSH32(0x%xu); M.pc = %s; return;' % (v, expr, self.i.end, v))

    def op_jmp(self):
        t, expr = self._target()
        if t is not None:
            self.s(self.branch(t))
        else:
            self.s('M.pc = %s; return;' % expr)

    def op_rts(self):
        self.s('M.pc = POP32(); return;')

    def op_rte(self):
        self.s('RTE(); return;')

    def op_rtr(self):
        self.s('SET_CCR(RW(M.a[7])); M.a[7] += 2; M.pc = POP32(); return;')


class Region:
    def __init__(self, start):
        self.start = start
        self.insns = []
        self.addrs = set()


def speculate(an, img):
    """Unreferenced code-like gaps: decoded linearly, they must stay inside the gap, end in a transfer and use no
    illegal opcode; such a gap is traced as an extra entry (docs/DECISIONS.md D12)."""
    dec = Decoder(img.read16)
    added = []
    changed = True
    while changed:
        changed = False
        for h, a, b in an.gaps(6):
            if img.hunks[h].kind != 'CODE' or a & 1:
                continue
            if h in (2, 3):
                continue                  # hunks 2 and 3 are data (docs/RECOMP-PLAN.md)
            x, n, ok = a, 0, False
            while x < b:
                try:
                    i = dec.decode(x)
                except (DecodeError, IndexError):
                    break
                if i.op in ('illegal', 'linea', 'linef', 'reset', 'stop', 'trap') or x + i.length > b:
                    break
                if i.op == 'ori' and img.read16(x) == 0:
                    break                 # zero words: data
                n += 1
                x += i.length
                if i.op in ('rts', 'rte', 'jmp', 'bra'):
                    ok = n >= 3
                    break
            if not ok:
                continue
            # trial trace on copies: keep only if it adds no error
            trial = Analysis(img)
            trial.insns, trial.owner = dict(an.insns), dict(an.owner)
            trial.trace(a, 'speculative')
            if trial.errors:
                continue
            an.entries.add(a)
            an.trace(a, 'speculative')
            added.append(a)
            changed = True
            break
    return added


def build(tx_path, outdir):
    img = load(tx_path)
    import hashlib
    sha = hashlib.sha1(open(tx_path, 'rb').read()).hexdigest()
    if sha != patches_maya.T_X_SHA1:
        print('warning: T.X SHA-1 %s differs from the checked v1.4 file; the patch checks decide' % sha)
    patches_maya.apply(img)
    an = Analysis(img)
    an.run(hints_maya.HINTS)
    if an.errors:
        for a, e in sorted(an.errors.items()):
            print('  $%06x %s' % (a, e))
        raise SystemExit('analysis errors')
    spec = speculate(an, img)

    # every address that is entered from outside its own straight-line flow starts a region
    starts = set(an.entries)
    for t in getattr(an, 'tables', []):
        pass
    addrs = sorted(an.insns)
    # regions: each entry up to the next entry; an instruction before any entry of its hunk starts one
    regions = []
    cur = None
    entry_set = starts
    prev_hunk = None
    for a in addrs:
        h = img.hunk_at(a).index
        if cur is None or a in entry_set or h != prev_hunk:
            cur = Region(a)
            regions.append(cur)
        cur.insns.append(an.insns[a])
        cur.addrs.add(a)
        prev_hunk = h

    os.makedirs(outdir, exist_ok=True)
    hooks = patches_maya.HOOKS
    code_lo = min(addrs)
    code_hi = max(an.insns[a].end for a in addrs)
    with open(os.path.join(outdir, 'tx_code.c'), 'w', newline='\n') as f:
        f.write('/* Generated by tools/m68k/gen_c.py from the user\'s T.X. Do not edit, do not commit. */\n')
        f.write('#include "cpu.h"\n#include "hooks.h"\n#include "tx_gen.h"\n\n')
        f.write('#if defined(__clang__)\n#pragma clang diagnostic ignored "-Wunused-label"\n'
                '#pragma clang diagnostic ignored "-Wunused-variable"\n#elif defined(_MSC_VER)\n'
                '#pragma warning(disable: 4102 4189 4146)\n#endif\n\n')
        hw_checks = 0
        for rg in regions:
            f.write('static void R_%06x(void)\n{\n    switch (M.pc) {\n' % rg.start)
            for i in rg.insns:
                f.write('    case 0x%xu: goto L_%06x;\n' % (i.addr, i.addr))
            f.write('    default: rt_bad_pc(); return;\n    }\n')
            hw_regs = hw_base_regs(rg.insns)
            for k, i in enumerate(rg.insns):
                e = Emit(None, i, rg)
                try:
                    body = e.emit()
                except GenError as ex:
                    raise SystemExit(str(ex))
                f.write('L_%06x: /* %s */\n    {\n' % (i.addr, str(i).replace('*/', '* /')))
                if touches_hw(i, hw_regs):
                    f.write('        if (M.cyc >= M.ev) { M.pc = 0x%xu; return; }\n' % i.addr)
                    hw_checks += 1
                f.write('        M.cyc += %d;\n' % cycles(i))
                if i.addr in hooks:
                    f.write('        %s();\n' % hooks[i.addr])
                for line in body:
                    f.write('        %s\n' % line)
                f.write('    }\n')
                nxt = rg.insns[k + 1].addr if k + 1 < len(rg.insns) else None
                if i.op not in FLOW_END and nxt != i.end:
                    f.write('    M.pc = 0x%xu; return;\n' % i.end)
            f.write('}\n\n')
        f.write('const TxRegion tx_regions[] = {\n')
        for rg in regions:
            f.write('    {0x%xu, 0x%xu, R_%06x},\n' % (rg.start, rg.insns[-1].end, rg.start))
        f.write('};\nconst unsigned tx_region_count = %d;\n' % len(regions))

    with open(os.path.join(outdir, 'tx_image.c'), 'w', newline='\n') as f:
        f.write('/* Generated by tools/m68k/gen_c.py: T.X loaded at $%x, relocated, WHDLoad v1.4 patches applied. '
                'Do not commit. */\n#include "tx_gen.h"\n\n' % img.start)
        f.write('const unsigned char tx_image[%d] = {\n' % len(img.mem))
        m = img.mem
        for k in range(0, len(m), 24):
            f.write('    ' + ','.join(str(b) for b in m[k:k + 24]) + ',\n')
        f.write('};\n')
        f.write('const TxHunk tx_hunks[] = {\n')
        for h in img.hunks:
            f.write('    {0x%xu, 0x%xu, %d},\n' % (h.base, h.size, {'CODE': 0, 'DATA': 1, 'BSS': 2}[h.kind]))
        f.write('};\nconst unsigned tx_hunk_count = %d;\n' % len(img.hunks))

    with open(os.path.join(outdir, 'tx_gen.h'), 'w', newline='\n') as f:
        f.write('/* Generated by tools/m68k/gen_c.py. Do not commit. */\n#ifndef TX_GEN_H\n#define TX_GEN_H\n\n')
        f.write('#define TX_IMAGE_BASE 0x%xu\n#define TX_IMAGE_SIZE 0x%xu\n#define TX_ENTRY 0x%xu\n'
                % (img.start, len(img.mem), img.hunks[0].base))
        f.write('#define TX_CODE_LO 0x%xu\n#define TX_CODE_HI 0x%xu\n#define TX_EXTER_INT 0x%xu\n\n'
                % (code_lo, code_hi, patches_maya.EXTER_INT))
        f.write('typedef struct TxRegion { unsigned lo, hi; void (*fn)(void); } TxRegion;\n')
        f.write('typedef struct TxHunk { unsigned base, size; int kind; } TxHunk;\n')
        f.write('extern const TxRegion tx_regions[];\nextern const unsigned tx_region_count;\n')
        f.write('extern const unsigned char tx_image[%d];\n' % len(img.mem))
        f.write('extern const TxHunk tx_hunks[];\nextern const unsigned tx_hunk_count;\n\n#endif\n')
    print('gen_c: %d instructions, %d regions (%d speculative entries), code $%x-$%x -> %s'
          % (len(an.insns), len(regions), len(spec), code_lo, code_hi, outdir))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    build(sys.argv[1], sys.argv[2])
