/*
 * hooks.c - the WHDLoad slave's data fix after T.X's unpacker (patches_maya.py HOOKS; docs/DECISIONS.md D8).
 * Portable C.
 */
#include "amiga.h"
#include "hooks.h"

/* At $46f9a the unpacker is done and about to restore d0-d7/a0-a6 from the stack.  The slave's replacement for
 * "tst.w d3 / bmi" scans the unpacked bytes (a1 + 1 .. a1 + d1, from the saved registers) for the first "L6:" + $14
 * and makes the $14 a $0a. */
void hook_fix_l6(void)
{
    uint32_t sp = M.a[7];
    uint32_t d1 = RL(sp + 4);
    uint32_t a1 = RL(sp + 36);
    uint32_t end = a1 + d1;
    uint32_t p = a1;
    for (;;) {
        p++;
        if (p == end || (p & 0xffffffu) + 4 > RAM_SIZE) {
            return;
        }
        if (g_ram[p & 0xffffffu] == 'L' && g_ram[(p + 1) & 0xffffffu] == '6' && g_ram[(p + 2) & 0xffffffu] == ':'
            && g_ram[(p + 3) & 0xffffffu] == 0x14) {
            g_ram[(p + 3) & 0xffffffu] = 0x0a;
            return;
        }
    }
}
