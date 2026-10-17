/* Opponent "cheat mode" (the C1 mechanism, kept in C2).
 *
 * CalcOpponentConspicuousnessWithAViewToCheatingLikeFuck (0x4a9550) stamps last_in_view while
 * an opponent is within gIn_view_distance (yon + 10, at least 45 units) of the player.
 * ProcessThisOpponent (0x4aa570): once now > last_in_view + 3000 ms it calls StartToCheat:
 * TurnOpponentPhysicsOff hides the car, drops it from PHIL and the active car list, and moves it
 * along the path nodes with no collision. Coming back into range rematerialises it snapped onto a
 * path section, which is how opponents end up stuck behind or inside barriers.
 *
 * [AI] OpponentCheating=0 makes the "in view" branch unconditional: opponents always run full
 * physics. [AI] CheatDelayMs changes the 3000 ms grace period instead.
 * (gIn_view_distance itself must stay small: TeleportOpponentToNearestSafeLocation loops until
 * it finds a path node farther than that from the player.)
 */
#include "c2patch.h"

void c2_ai_install(void)
{
    static const uint8_t add_3000[] = {0x05, 0xb8, 0x0b, 0x00, 0x00};       /* 4aa598 add eax,3000 */
    static const uint8_t jbe_in_view[] = {0x0f, 0x86, 0xf8, 0x01, 0x00, 0x00}; /* 4aa59f jbe 4aa79d */
    static const uint8_t jmp_in_view[] = {0x90, 0xe9, 0xf8, 0x01, 0x00, 0x00}; /* nop; jmp 4aa79d */
    int delay;

    if (!c2_ini_int("AI", "OpponentCheating", 0)) {
        if (c2_patch(0x4aa59f, jbe_in_view, jmp_in_view, sizeof jmp_in_view)) {
            c2_log("ai: opponent cheat mode disabled (always full physics)");
        }
        return;
    }
    delay = c2_ini_int("AI", "CheatDelayMs", 3000);
    if (delay != 3000 && c2_expect(0x4aa598, add_3000, sizeof add_3000)) {
        c2_write32(0x4aa599, (uint32_t)delay);
        c2_log("ai: cheat delay %d ms", delay);
    }
}
