# Network player limit

**The stock cap is 12** (`kMax_netplayers`). The manual's "2-6 players" is only a recommendation.
I swept every instruction for comparisons of `gNumber_of_net_players` (0x74b7d4) or
`num_players` (+0x34 of `tNet_game_details`) against small immediates. Only the 12 gates below exist.

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
