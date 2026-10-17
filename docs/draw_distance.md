# Distance culling

"Units" are BRender world units. Patched mechanisms are marked with their `c2patch.ini` key.

## Opponent cheat mode: the "stuck behind barriers" cause (`[AI] OpponentCheating`)
- CalcOpponentConspicuousnessWithAViewToCheatingLikeFuck 0x4a9550 stamps `last_in_view` (+0x68) while the
  opponent is within `gIn_view_distance` (0x691764) of the **player car**. InitOpponents 0x4add50 sets it to
  `gCamera_yon + 10`, with a minimum of 45.
- ProcessThisOpponent 0x4aa570: `if (now > last_in_view + 3000)` (`add eax,0xbb8` @0x4aa598, `jbe` @0x4aa59f),
  it calls StartToCheat. TurnOpponentPhysicsOff 0x4a79a0 hides the car (render_style NONE) and drops it from
  PHIL and the active car list (RebuildActiveCarList 0x4a7a80). It then slides along the path nodes with no
  collision, at 6 units/s, and may use cheat-only path sections.
- Coming back into range, RematerialiseOpponentOnThisSection 0x4a7f20 snaps it onto the path segment, possibly
  inside or behind a barrier.
- Patch: `jbe` → `nop; jmp`, so the "in view" branch (OiStopCheating) always runs.
- Don't make `gIn_view_distance` huge: TeleportOpponentToNearestSafeLocation 0x4a9180 loops until it finds a node
  farther away than that.

## Grooves (moving scenery) freeze out of view (`[Render] AnimateAllGrooves`)
- GrooveThisDelic 0x478c80 checks `PointOutOfSight(pos, mode==1 ? gYon_squared : 36.0)` (PointOutOfSight 0x4e5ce0:
  far, **or behind the camera**). If the groove is out of sight, it skips both PathGrooveBastard and
  ObjectGrooveBastard.
- Collision uses the actor's current matrix, so a frozen gate or lift blocks the AI wherever it stopped.
- Patch: NOP the `jnz` @0x478cd8.

## Track columns (what visibly pops out) (`[Render] MinYonFactor`)
- RenderTrack 0x40d7c0 adds only the columns under the view frustum out to `yon_z * gYon_factor` (0x655e60, default 0.25),
  plus one column of margin. The scenery and static non-car objects live in those columns.
- SetYonFactor 0x40dff0 (stdcall) is the only writer. It is called from the menu (1/0.5/0.25/0.125), RestoreOptions
  (OPTIONS.TXT `YonFactor`) and the CycleYonFactor hotkey.
- Collision doesn't depend on it (FindFacesInBox walks the columns around each car).

## Far clip (`[Render] MinYon`)
- SetYon 0x446b70 (stdcall, floor 5.0 @0x589d1c) writes `gCamera_list[0..1]->type_data->yon_z` and `gCamera_yon` 0x761f4c.
  The menu offers 20-35; OPTIONS.TXT `Yon` can be anything.
- AssertYons 0x446a10 sets `yon_z = gYon_multiplier (track TXT, 0x655ddc) * gCamera_yon`.
- CalculateCameraStuff 0x4940e0: `gYon_squared = mult^2 * yon_z^2`, so the multiplier is applied twice. It is used by
  MungeCarGraphics (hides net/player cars out of sight), grooves, and powerup respawn effects.

## Not patched / visual only
- Drones (traffic, trains, planes): DoDroneDistanceChecks (from DoDronePerGameFrameStuff 0x4512f0) stops processing them
  outside a box around the camera (car 18x13x18, plane/train 43x31x43), which drops them from PHIL.
- Peds: `gPed_process_distance`/`gPed_popup_distance` from PEDS\SETTINGS.TXT.
- Net clients only simulate other net cars within 10 units (IsNetCarActive).
- Non-car objects: no distance cutoff found (APTCPreCollision simulates all active non-cars).
- ActorBoxPick 0x45f8c0 skips actors with render_style NONE, so `AccessoryRendering 0` removes accessory collision for everyone.
