# Network player limit

**The stock cap is 12** (`kMax_netplayers`). The manual's "2-6 players" is only a recommendation.
I swept every instruction for comparisons of `gNumber_of_net_players` (0x74b7d4) or
`num_players` (+0x34 of `tNet_game_details`) against small immediates. Only the 12 gates below exist.

## Tested: 12 players work (tools/mp.py, 2026-10-17)
Twelve instances on one PC (nettest harness, IPX loopback) were tested end to end:
- **Lobby:** host + 11 joins; the NetSync list shows all 12; the lobby **auto-starts when the 12th player joins** (0x473c78).
- **Race start:** StartRace with 12 racers.
- **Starts:** 12 grid slots in Sudden Death on the city track, and 12 scattered starts in THE ARENA.
- **HUD:** a 12-row scoreboard that fits.
- **Racing:** minutes of bumper cars at 60, 10 and 5 fps; no instance died.

### Bug: crush-list overflow with 8+ players (fixed by `[Net] FixCrushListOverflow`)
`gCrush_lists[8]` (0x6796c8, 0x158 each) collects the cars that took crush damage this frame. It is cleared once per
rendered frame by CrushBendFlapRend (0x436170).
- **Add path** (FUN_0042d400): searches up to 8 slots and may fill slot 7 (`cmp edi,8` @0x42d714).
- **Consumers:** the loop @0x4361b9 and the clear loop @0x4363d2 walk until `car_spec == NULL` with **no bound**.
- **With 8 slots used:**
  - the consumers run into `gSplit_car_collision_infos` (heap ptr) and `gMin_bend_angle` (30.0f, an item count of 0x41f00000);
  - the clear loop zeroes `gSplit_car_collision_infos`;
  - the next add's search (edi=8) treats those globals as an existing entry and writes past the array.
- **Measured** (nettest probe at 0x42d714): only the **host** processes crush for many cars. In head-on pile-ups of 12 cars in
  THE ARENA it reached 5 distinct cars in one frame at 10 fps; clients stayed at 2-3. Lower frame rates (period-correct
  hardware) put more physics steps into each frame, so 8 is reachable in big games, and it takes down the host.
  AI cars far from the camera are skipped, so single player effectively never gets there.
- **Fix:** limit the list to 7 entries (`cmp edi,7`), so slot 7 always stays NULL. An 8th car's crush waits one frame.

## Update rate (`src/netrate.c`)
- **Physics** runs at 25 Hz: PHILDoPhysics (0x4b6650) snaps `gPHIL_last_physics_tick` (0x74a5ec) to 40 ms and
  steps `+= 0x28` (@0x4b6ae2).
- **When car data goes out:** ApplyPhysicsToCars (0x416340) calls TimeToSendData (0x4978d0) once per frame after
  the physics steps, then SendCarData (0x497900) + SendMines.
  - **Stock gate:** send when `tick >= last + 0x50` (80 ms, 12.5 Hz), or when `tick <= last`.
  - **Stock duplicates:** that second case fires on every frame without a new physics step, resending identical data
    with the same timestamp. Measured: about 30 packets/s at 60 fps for 12.5 distinct updates/s. Below 12.5 fps,
    it sends once per frame.
- **Who sends what:** the host broadcasts every player's car (plus a 0x12 race-time chunk); a client sends only
  its own car. So the host's rate decides how smoothly everyone sees the other players.
- **Settings** (`[Net]`):
  - `SendIntervalMs`: 40 / 80 / 120 measured 25.0 / 12.6 / 8.3 sends/s.
  - `SkipDuplicateSends=1`: drops the duplicates.
  - **In game:** NET GAME OPTIONS has an UPDATE RATE row (25 HZ / 12 HZ / 8 HZ, radio group 8, items 31-34; OK/CANCEL
    moved to 35/36; rows re-spaced from 28 to 24 px), added at runtime by hooking the screen's create callback
    (`gFrontend_NETWORK_OPTIONS`+0x108, before FRONTEND_CreateMenu builds the text actors). OK saves to c2patch.ini.
  - The setting is per machine: `tNet_game_options` has no spare field to carry it to clients.
- **Stress test:** 12 players at 25 Hz in THE ARENA under bumper-car chaos, no deaths.

| addr | where | what |
|---|---|---|
| 0x4a08a4 | ReceivedJoin (inlined in ReceivedMessage 0x4a0260) | host rejects a joiner if `num_players >= 12` (reject path at 0x4a0b05) |
| 0x468301 | NetJoinChooseThisGame | client can't pick a game with `num_players >= 12` |
| 0x49e33f, 0x49ebec | NetHostGame, NetJoinGame | loops over all 12 `gNet_players` slots (`cmp esi,0x74c6f8`) |
| 0x499183 | SetUpNetCarPositions | grid index search `j < 12`, FatalError if no slot |
| 0x473720, 0x473c1d/2f, 0x473c78 | NetSync screen | 12 name/status rows; auto-start when 12 players |
| 0x4e2d84, 0x4e2e9b | racestrt routine 0x4e2cb0 | same as the NetSync logic |
| 0x474288/9a, 0x4744cf/e1 | NetSummary screen | hide-unused loops |
| 0x499acc, 0x499adc, 0x499b1f, 0x49a2ef | DoNetScores2 | stack array of 12 x 12 bytes, qsort n=12, HUD boxes 0x28 px apart |
| 0x497f01, 0x49837d, 0x498433, 0x498731, 0x498793 | crush network shapes | `tCar_crush_network_shapes.items[12]`, allocated as 0x60 (0x42a9cc, 0x42af87) |
| 0x468506 … 0x4690cc (6x) | InitNetStorageSpace (inlined) | clears `opponent_list[0..11]` (`cmp eax,0x763150`) |

## What going past 12 would need
1. **StartRace message (0x0c), the hard wall.** SignalToStartRace 0x498e70 sends one chunk of
   6 + racers x 0x14 bytes = 0xf6 for 12 racers. Chunk sizes are a `tU8`, and NetGetContentsSize 0x49f440
   fatal-errors above 0xff (`cmp esi,0xff` @0x49f54c). 13 racers means 0x10a bytes, so the message format
   (sender and receiver) would have to be redesigned, for example split into two chunks.
2. **Relocate `gNet_players[12]`** (0x74bcc0..0x74c6b0, 0xd4 each). It is packed between `gCar_details` and
   `gFrontend_net_initialized`/`gViewable_car_list`/`gCamera_list`, and there are about 445 absolute references
   (no relocation table, so they must be found by scanning). Loop bounds like 0x74c6f8 are end-plus-field-offset
   values that alias the globals after the array, so each one needs manual review.
   **`gNew_net_players[12]`** (0x690240..0x690c30, the client's incoming player list) needs the same.
   ReceivedNewPlayerList 0x4a48d0 copies to `[msg+8]` with no bounds check.
3. The crush-shape allocation and loops, the DoNetScores2 stack frame, and the qsort count.
4. The frontend: `INTRFACE\netsync.txt` (28 items = 2 + 12 names + 12 statuses + 2 buttons) and
   networksummary.txt (80 items), with item indices hard-coded (`2+i`, `14+i`, columns `0/12/24/36/48/65+i`).
   `tFrontend_spec.items[100]` limits the summary to about 15 players, and 26 px rows fit about 14 on a 480-line screen.
5. Messages that are fine: GameScores 0x1e (<= 20), Scores 0x18 (bitmask `1<<i`, so <= 32), Join and NewPlayerList (one per player).
   Datagrams are 512 bytes (`gSend_buffer`/`gReceive_buffer`, `recvfrom(…,512)`).
6. Car storage `gNet_cars_storage_space` (0x761f00: 2000 pixelmaps/materials/models, 50 shade tables/sounds) may run out with more distinct cars.

Realistic ceiling without a protocol redesign: **12**. With items 1-4, the UI tops out around 14.

## Driver name length (`src/names.c`)
- **Where the 8 comes from:** ToggleTyping (0x466ec0) inlines `StartGettingInputString(text, 8)`
  (`mov [gFrontend_maximum_input_length], 8` @0x46701a). It is the only writer; the main menu and the network
  screen's DRIVER field both use it. Typed characters come from WM_CHAR (gKeyboardBuffer); Backspace and Enter come
  through the key array.
- **Where the name is stored** (all hold 13 characters plus the terminator, or more):
  - `gProgram_state.player_name[14]` (0x75d590, followed by `track_file_name`);
  - `tSave_game.player_name[14]`;
  - `tNet_game_player_info.player_name[32]` (what the network carries);
  - `tCar_spec.driver_name[32]` (bounded memcpy in LoadCar);
  - frontend item text[255].

  Copies into display buffers are 128-256 bytes, and the taunt message checks `len <= 18`.
- **Setting:** `[Player] MaxNameLength` (default 13, the most without moving `player_name`).
- **Tested** with 13-character names (`WWWWWWWWWWWWW`, `Carmageddon13`, `Stainless1998`):
  - typing stops at 13 and the next field is untouched;
  - the name is saved to OPTIONS.TXT and reloaded;
  - the network lobby shows the full names on every machine.

  Cosmetic: the in-race scoreboard boxes and the PLAYER: status line clip long names by pixel width (8-10
  characters show), and 13 W's run slightly past the main menu's panel edge.
- `DefaultNetName` (0x49bd10) still asks for the Windows user name with a 32-byte limit into the 14-byte field,
  then cuts it to 9 characters (an existing, unrelated overflow on first run).
