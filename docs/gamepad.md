# SDL gamepads (`src\gamepad.c`, `[Gamepad]`)

## How the game reads joysticks
DirectInput 5, in the platform code around 0x459000 (see `win32_dinput.c` in the reference decomp):

- `JoystickDInputBegin` 0x458860 → `EnumDevices(DIDEVTYPE_JOYSTICK)` once; `Win32DInputJoystickEnum` 0x459c70
  creates each device, sets `c_dfDIJoystick`, QI's `IDirectInputDevice2A`, sets DISCL_BACKGROUND|EXCLUSIVE,
  DIPROP_RANGE 0..0xffff on X and (by mistake) passes the range struct as DIPROP_DEADZONE. A failing
  SetProperty drops the device.
- `gJoystick_index` 0x595f88 = 0 after enumeration (the first device listed); -1 = joystick disabled.
- Per frame `GetDirectInputJoy1X` 0x459050 / `Joy1Y` 0x459370 / `PDGetJoystickButtonStates` 0x459690 each do
  `Poll` (anything but DI_OK is an error) + `GetDeviceState(sizeof(DIJOYSTATE))`.
  X/Y are scaled by the sensitivity sliders around 0x7fff and stored in `gJoy1_x` 0x68be44 / `gJoy1_y` 0x68c13c.
  Y below 0x8000 accelerates, above brakes/reverses.
- `PDSetKeysFromJoystick` 0x45b360: `rgbButtons[i]` → `gKey_array[107 + i]` for 32 buttons ("Joy 1 B1".."B32" in
  `DATA\KEYNAMES.TXT`), so pad buttons are bindable in Configure Controls.
- Force feedback needs `DIDC_FORCEFEEDBACK` in the caps; then `InitForceFeedback` 0x457760 loads `FFB.IFR`
  through I-Force (IFORCE2.dll) and fails fatally if that doesn't work. The virtual device reports none.

## The patch
- Wraps the IDirectInputA vtable (CreateDevice, EnumDevices; the vtable is shared, so once) in the dinput proxy,
  or through the IAT as an Interposer plugin.
- EnumDevices adds a virtual `IDirectInputDevice2A` ("SDL: <pad name>"), first if an SDL gamepad is connected,
  last otherwise (so a wheel stays joystick 0). CreateDevice with its GUID returns the static object.
- SDL3.dll, else SDL2.dll, is loaded on the first EnumDevices (game thread, not DllMain), with
  `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`, `SDL_JOYSTICK_THREAD=1` and `SDL_JOYSTICK_RAWINPUT=0`.
  RAWINPUT is off because, embedded in a non-SDL game, SDL owns no window pumping `WM_INPUT`, so its
  RAWINPUT backend never correlates an Xbox-class pad with XInput: axes/buttons read 0 and rumble fails
  with "isn't correlated yet". With it off SDL polls XInput directly, so input and rumble work at once
  (the fallback backends still cover non-XInput devices — a wheel, or the pad in DInput/Switch mode).
  `pad_update` (at most every 2 ms) calls
  the gamepad update, flushes the unread event queue, reopens the first pad after a disconnect (scan once a second).
- State: X = left stick, Y = LT − RT (or the left stick, `Throttle`), Z = LT, Rx/Ry = right stick, Rz = RT,
  POV = D-pad, buttons 1..21 = SDL's gamepad buttons, 22/23 = LT/RT past half.
- Menu keys (`MenuControls`): the keyboard GetDeviceState (vtable slot 9 of the real devices) gets arrows /
  Return / Escape ORed in while a menu runs: `gProgram_state.racing` 0x75bba8 is 0, or the innermost
  `FrontEndProcessMenus` 0x46d8e0 was entered while racing (the pause menu; `gCurrent_frontend_spec` 0x688abc
  is not cleared when a menu closes, so it can't tell). Racing outside a menu: Start = Escape.
  Not inside `Controls_SlotActivated` 0x472b00 (waits for the key to bind), so a pad button binds as Joy 1 Bn.
  Both functions are hooked at their entry (moved prologue in a trampoline: `mov eax,[0x762328]` and
  `sub esp,0xc; mov eax,[0x688770]`).

## In-race button actions (`ButtonActions`)
Out of the box the pad drives the race with a Carmageddon: Max Damage layout (its default gamepad preset,
translated to C2 actions). Accelerate/brake/steer are the triggers and left stick (the axes above); the
buttons fire game actions through the same keyboard-injection path as the menu keys — in the race branch of
the `hk_GetDeviceState` hook (not in a menu, not binding), each held pad button ORs its action's **current**
keyboard scancode into the DirectInput keyboard buffer. So it is additive (keyboard still works) and follows
any Configure Controls rebind, rather than rewriting the key map.

Every action — the rebindable driving keys, the `gToggle_array` toggles, and the APO buys — is reached the
same way: `gKey_array[gKey_mapping[slot]]` is set from `diKeys[gScan_code[gKey_mapping[slot] + 12]]`. So the
injector just needs the action's `gKey_mapping` slot and reads the live scancode from
`gScan_code` 0x6b3470 (int[256]) indexed by `gKey_mapping` 0x74b5e0 (int[77]). Values ≥ 107 (joystick
buttons/axes) have no scancode and are skipped, so an action the user rebound to a pad button doesn't
double-fire.

| Pad | C2 action | gKey_mapping slot | default key |
|-----|-----------|-------------------|-------------|
| B | hand brake | 54 | space |
| X | repair | 45 | backspace |
| Y | recover | 60 | insert |
| A | open/activate power-up inventory | 62 | `;` |
| LB / RB | previous / next power-up | 63 / 64 | `[` / `]` |
| left-stick click | horn | 59 | h |
| right-stick click | map | 57 | tab |
| D-pad ← / ↑ / → | buy Armour / Power / Offense | 67 / 68 / 69 | Del / End / PgDn |
| Start | pause (`MenuControls`) | — | esc |

The D-pad buys, repair and recover spend credits, so they no-op with a "too poor" message when you can't
afford them — the action still fired. The menu-depth/binding hooks (above) go in whenever `MenuControls` *or*
`ButtonActions` is on, so `menu_active()` stays right and the buttons never fire inside the pause menu. Set
`ButtonActions=0` to leave the pad buttons as plain Joy 1 Bn for binding by hand.

## Rumble (`Rumble`, `ImpactScale`)
The game's force-feedback calls happen whether or not a force-feedback device exists; the effect functions return
early on `gForceFeedbackAvailable` 0x686168 == 0. Hooked at their entries:
- 0x45c910 (fastcall ecx, edx + 2 stack args, `ret 8`; called only from 0x41d351, for the car whose driver is the
  local human): the 2nd stack argument is the impact (≥ 0.02). The function clamps it to the FFB.TXT limits
  (3.5..4.7, at 0x655e5c/0x655e58) and calls the collision force 0x458720 (magnitude 0..10000, direction in 1/100°).
  Because of the clamp, every wheel kick is 74–100%. The rumble uses impact / ImpactScale instead.
  Measured in bumper cars (4 units, 60 s): ~9 impacts a second per car, median 0.13 (scraping), p90 0.16,
  p99 1.0, max 3.7. Below 5% of the scale nothing rumbles.
- 0x45c6b0 PlayExclusiveFFBEffect(name, flag) (the decomp calls it `TriggerPedestrianReaction`): Afterburner
  (SetEngineFactor, SetOpponentsSpeed), Mine_Shit, Oil_Slick, Kangeroo, Ped_Ray, Flamer, Repulse.
  0x45c720 PDPlayFFBEffect(name): Instant_Handbrake (PollCarControls). Each name has a fixed rumble pattern.
- Other FFB users that aren't hooked: CalcForce's steering force (0x4586b0) and friction (0x458660). Those are
  continuous wheel forces with no rumble equivalent.
- A rumble fades out linearly. `pad_update` re-sends the level every 30 ms, and a new rumble replaces the
  current one only if it is stronger than what remains.

## Diagnosing "no rumble" (c2patch.log)
The virtual test pad has no motors, so rumble can only be verified from a real controller via the log. Each
prerequisite is logged under `gamepad:`; read them in order:
- `no SDL3.dll or SDL2.dll next to the game` — the pad (and rumble) run through SDL; drop a 32-bit SDL DLL
  next to CARMA2_HW.EXE. If a controller still works, it is going through the game's native DirectInput, which
  this patch cannot rumble.
- `rumble unavailable (SDL too old...)` — the SDL DLL predates `SDL_GameControllerRumble` (SDL2 < 2.0.9).
- `using <pad>` / `listed to the game as ...` — SDL opened the controller. No such line means SDL did not see
  it as a gamepad (a wheel or an exotic HID); `HideDirectInputJoysticks=0` keeps the native device usable.
- `<pad> reports NO rumble` — the controller/driver tells SDL it has no motors.
- `SDL rumble failed on <pad>: <reason>` — the reason is SDL's. Xbox pads on SDL's RAWINPUT backend say
  "Controller isn't correlated yet, try hitting a button first": rumble starts working once SDL has seen input,
  so this on the connect pulse is harmless and it should clear once driving. Logged at most once per 5 s.
- `rumble reached <pad>` — SDL accepted the rumble; if you still feel nothing it is the controller/driver.
On opening a pad, a 300 ms test pulse fires (`reports rumble; test pulse`) so hardware can be checked without
driving. Per-impact magnitudes are logged only under `C2_UNIT`.

## Tested
Physical pad (8BitDo in XInput mode, SDL2, dinput.dll proxy): driving input (left stick / triggers),
rumble, and the full `ButtonActions` layout all confirmed in a race. The log shows `rumble reached` on the
connect pulse (felt as a buzz at launch) and in a race. This is the fix in the bullet above: before
`SDL_JOYSTICK_RAWINPUT=0` the same pad opened but every axis read 0 and rumble failed "isn't correlated yet"
— the symptom of the uncorrelated RAWINPUT backend. A virtual pad (Steam Input / X360 bus) also presents as
"Xbox One Controller"; close it so SDL opens the real device.

nettest virtual pad (SDL2 2.x from MSYS2): driving flat out into walls gave 37 rumble starts in 14 s, and
RT/LT/steering reach `gJoy1_x/y` (0xffff right, Y 0 full throttle). The virtual pad's rumble call fails
("isn't correlated yet") because it is not real hardware, so it exercises the event→SDL path only.
Main menu up/down/select/back; Start opens the in-race Options menu, D-pad moves in it, B returns to the
race; in Configure Controls, A starts a binding and X binds as JOY 1 B3. Without SDL DLLs the log says so
and nothing changes.
Not tested: SDL3, the Interposer plugin path.
