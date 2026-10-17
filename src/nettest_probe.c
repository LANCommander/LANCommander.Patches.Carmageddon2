/* nettest probe: how full does gCrush_lists[8] (0x6796c8) get?
 *
 * FUN_0042d400 adds one entry per crushed car per frame: at 0x42d714 `cmp edi,8; jge 0x42d759`
 * guards the store of slot edi. The consumers (CrushBendFlapRend 0x4361b9, its clear loop
 * 0x4363d2) walk until car_spec == NULL with no bound, so a full list (slot 7 used) runs into
 * gSplit_car_collision_infos / gMin_bend_angle. This records the highest slot used and how
 * often slot 7 was filled; nettest_input.c logs them.
 */
#include "c2patch.h"

volatile int c2_probe_crush_max = -1;
volatile int c2_probe_crush_slot7;
int c2_crush_limit = 8;    /* 7 with [Net] FixCrushListOverflow */

void c2_probe_crush_stub(void);
__asm__(
    ".globl _c2_probe_crush_stub\n"
    "_c2_probe_crush_stub:\n"
    "    cmpl _c2_probe_crush_max, %edi\n"
    "    jle 1f\n"
    "    movl %edi, _c2_probe_crush_max\n"
    "1:  cmpl $7, %edi\n"
    "    jne 2f\n"
    "    incl _c2_probe_crush_slot7\n"
    "2:  cmpl $8, %edi\n"            /* original: cmp edi,8 / jge 0x42d759 */
    "    jl 3f\n"
    "    pushl $0x42d759\n"
    "    ret\n"
    "3:  pushl $0x42d719\n"
    "    ret\n");

void c2_probe_install(void)
{
    static const uint8_t orig[] = {0x83, 0xff, 0x08, 0x7d, 0x40};
    if (c2_expect(0x42d714, orig, sizeof orig)) {
        c2_jmp(0x42d714, c2_probe_crush_stub);
        c2_log("probe: crush list fill instrumented");
    }
}
