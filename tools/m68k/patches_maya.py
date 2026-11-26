"""The WHDLoad v1.4 slave's patches to T.X (Maya.Slave by Mr.Larmer / CFou!, decoded 2026-11-26), applied to the image
before analysis and code generation.  Every patch checks the original bytes first: a different T.X fails loudly.

Addresses are those of tools/m68k/image.py's layout (hunk 0 at $40008).  The slave addresses each site as an offset
from a hunk's segment link longword (BPTR*4 = hunk base - 4).  docs/DECISIONS.md D8 lists what is applied and why.
"""

# The CIA-B timer server's Interrupt structure (the slave's own at $101420); the runtime places it here and adds it
# to the EXTER server chain at boot, as the slave's AddIntServer(INTB_EXTER) does.  runtime/amiga_map.h must agree.
EXTER_INT = 0x00F01000

T_X_SHA1 = '393c2c3ed7e02bc8d0abffe14e61d1a947a9559a'   # the v1.4 data/T.X these patches were checked against

PATCHES = [
    # (address, original bytes, new bytes, note)
    (0x4009c, '6600000a', '6002000a',
     'bne -> bra.s: take the PAL path ($47949 = 1); NTSC would spin forever at $400ca (slave: $98(a0) = $6002)'),
    (0x40954, '4eb900041938', '600400041938',
     'skip jsr $41938, the copy-protection disk track check (slave: $950(a0) = $6004)'),
    (0x49334, '22780004', '60060004',
     'sound player: skip ExecBase->IntVects[EXTER] walk to the ciab.resource internals (slave: hunk4+$45c)'),
    (0x4933c, '22512269000e', '227c%08x' % EXTER_INT,
     'movea.l #Interrupt,a1: the slave\'s own EXTER server (hunk4+$464)'),
    (0x4934e, '0044', '0012', 'handler stored at is_Code (18) instead of the resource vector (68) (hunk4+$476)'),
    (0x49368, '002900010028', '600400010028', 'skip the resource enable mask write (hunk4+$490)'),
    (0x493c2, '022800fe0028', '600400fe0028', 'skip the resource enable mask write (hunk4+$4ea)'),
    (0x49426, '022800fe0028', '600400fe0028', 'skip the resource enable mask write (hunk4+$54e)'),
    (0x49484, '002800010028', '600400010028', 'skip the resource enable mask write (hunk4+$5ac)'),
    (0x49574, '0044', '0012', 'next handler at is_Code (hunk4+$69c)'),
    (0x495b4, '0044', '0012', 'next handler at is_Code (hunk4+$6dc)'),
    (0x49d40, 'f002', 'f096', 'move.w #$400,$dff002 (read-only DMACONR) -> $dff096 DMACON (hunk5+$2d9 = $96)'),
]

# Code hooks: C functions the generated code calls before the instruction at the address (runtime/hooks.c).
HOOKS = {
    # The slave's fix after the unpacker loop (jsr at $46f64 replacing tst.w d3 / bmi): when the loop ends, the
    # first "L6:" followed by $14 in the unpacked data [a1+1, a1+d1) gets $0a instead (a text control byte).
    0x46f9a: 'hook_fix_l6',
}


def apply(img):
    import hashlib
    for addr, old, new, note in PATCHES:
        o = addr - img.start
        cur = bytes(img.mem[o:o + len(old) // 2]).hex()
        if cur != old:
            raise SystemExit('T.X patch at $%x: expected %s, found %s (not the WHDLoad v1.4 T.X?)' % (addr, old, cur))
        img.mem[o:o + len(new) // 2] = bytes.fromhex(new)
    return img
