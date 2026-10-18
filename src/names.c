/* Player name length.
 *
 * Typing a driver name stops at 8 characters: ToggleTyping (0x466ec0; the main menu, new game
 * and network screens all use it) inlines StartGettingInputString with a maximum of 8
 * (`mov [gFrontend_maximum_input_length], 8` @0x46701a). Every copy of the name holds 14 bytes or
 * more: gProgram_state.player_name[14] (0x75d590), tSave_game.player_name[14],
 * tNet_game_player_info.player_name[32] (what the network sends), the frontend item text[255].
 * So up to 13 characters fit without moving data or changing the save/network formats.
 * [Player] MaxNameLength (1..13, default 13).
 */
#include "c2patch.h"

void c2_names_install(void)
{
    static const uint8_t mov_max_8[] = {0xc7, 0x05, 0xd4, 0xb0, 0x59, 0x00, 0x08, 0x00, 0x00, 0x00};
    int len = c2_ini_int("Player", "MaxNameLength", 13);

    if (len < 1) {
        len = 1;
    } else if (len > 13) {
        len = 13;       /* player_name is char[14] in the program state and save games */
    }
    if (len == 8 || !c2_expect(0x46701a, mov_max_8, sizeof mov_max_8)) {
        return;
    }
    c2_write32(0x467020, (uint32_t)len);
    c2_log("names: driver names up to %d characters", len);
}
