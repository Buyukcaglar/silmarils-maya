"""Per-game facts for T.X (Le Fetiche Maya, Amiga, ALIS v1.3) that static analysis cannot derive.
Each entry says how it was established.  Addresses assume image.LOAD_BASE = $40000."""
HINTS = {
    'jump_tables': [
        # main opcode dispatch: sub_04200a does move.b (a3)+,d0 / add d0,d0 / move.w 0(a2,d0.w),d0 / jmp 0(a2,d0.w)
        # with a2 = $41c12 (lea at $41f1e).  214 entries ($00..$d5): the next table (a5's) starts at $41dbe =
        # $41c12 + 2*214; reading further gave overlapping-flow errors, 214 gives none.
        (0x41c12, 214, 'rel16'),
        # operand readers, indexed by the operand byte itself (codes are even: no add d0,d0):
        # a5 = $41dbe (lea at $41f22), used by sub_045eba: move.b (a3)+,d0 / move.w 0(a5,d0.w),d0 / jmp 0(a5,d0.w)
        (0x41dbe, 83, 'rel16'),
        # a0 = $41e64 (lea at $42304, sub_0422ee) and a0 = $41ea0 (lea at $42334, sub_04231e); they end where the
        # next table / the code at $41ee0 begins
        (0x41e64, 30, 'rel16'),
        (0x41ea0, 30, 'rel16'),   # ends at $41edc, which is an instruction (jmp $40442, the target of bra at $42848)
        # sound player (hunk 4): absolute tables read with movea.l tbl(pc,d1.w),a5 / jmp (a5); each ends where code
        # resumes ($49630 after $49620's four, $49710 after $49704's three, $497de after $497d2's three)
        (0x49620, 4, 'abs32'),    # movea.l $49620(pc,d1.w),a5 at $495ee
        (0x49704, 3, 'abs32'),
        (0x497d2, 3, 'abs32'),
        # sub_0469f8 / sub_046c96 reuse $41e64 / $41ea0 with ext.w (negative indexes possible in principle; the
        # runtime dispatcher resolves every indirect jump dynamically and reports unknown targets)
    ],
    'entries': [
        0x40a94,   # level-3 interrupt (VBL/copper/blitter): move.l #$40a94,$6c.w at $40a34
        0x49092,   # level-4 interrupt (audio): move.l #$49092,$70.w at $40980 (hunk 4 = sound/music player)
        0x49496,   # level-4 interrupt (audio, sample player): move.l #$49496,$70.w at $4931e
        # sound player timer handlers, chained by storing their address at 68(an) of its interrupt structure
        0x49590,   # move.l #$49590,68(a1) at $49348
        0x49538,   # move.l #$49538,68(a5) at $495ae
        0x49578,   # move.l #$49578,68(a5) at $4956e
    ],
}
