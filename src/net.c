/* Network games with 8+ players.
 *
 * The stock cap is already 12 (kMax_netplayers); see docs/network_players.md.
 *
 * [Net] FixCrushListOverflow: gCrush_lists[8] (0x6796c8, 0x158 each) collects the cars that took
 * crush damage this frame. FUN_0042d400 may fill all 8 slots (`cmp edi,8` @0x42d714), but
 * CrushBendFlapRend (loop @0x4361b9) and its clear loop (@0x4363d2) walk until car_spec == NULL
 * with no bound. With 8 slots used they run on into gSplit_car_collision_infos (a heap pointer)
 * and gMin_bend_angle (30.0f, read as an item count of ~1.1e9) and crash, and the clear loop
 * zeroes gSplit_car_collision_infos. AI cars far from the camera are skipped, so single player
 * never gets there; with 8+ network cars, 8 cars being crushed in one frame is enough.
 * The fix lets the list use 7 slots, so slot 7 always stays NULL as the terminator; an 8th
 * crushed car is picked up the next frame.
 */
#include "c2patch.h"

void c2_net_install(void)
{
    static const uint8_t cmp_edi_8[] = {0x83, 0xff, 0x08, 0x7d, 0x40};
    static const uint8_t limit_7 = 0x07;

    if (!c2_ini_int("Net", "FixCrushListOverflow", 1)) {
        return;
    }
    if (c2_expect(0x42d714, cmp_edi_8, sizeof cmp_edi_8)) {
        c2_write(0x42d716, &limit_7, 1);
    } else if (*(const uint8_t *)C2_ADDR(0x42d714) == 0xe9) {
        c2_crush_limit = 7;   /* the nettest probe owns that site; it compares against this */
    } else {
        c2_log("net: 0x42d714 has unexpected bytes; crush list fix not applied");
        return;
    }
    c2_log("net: crush list limited to 7 entries (8+ player crash fix)");
}
