# c2patch — Carmageddon II runtime patch

A small native patch for **Carmageddon II** that fixes multiplayer bugs and adds modern
conveniences — bigger draw distance, full-physics opponents, a configurable network send rate,
longer driver names, and full **game-controller support with rumble** — without modifying the game
executable. It applies guarded in-memory patches at startup, so a mismatched exe fails safe.

It is built the same way as the other [LANCommander](https://github.com/LANCommander) game patches
(e.g. [Battlefield 1942](https://github.com/LANCommander/LANCommander.Patches.Battlefield1942)): one
C core, three interchangeable delivery modes, settings in an INI next to the game and in the
LANCommander Interposer's `Config.yml`.

## Supported game

Only the GOG release **`CARMA2_HW.EXE`** (the 1998 MSVC build, SafeDisc removed, PE timestamp
`0x3662d4f7`, image base `0x400000`). Any other executable is detected and left untouched — no
patches are applied. Network play is IPX (over GOG's `winmm`/ipxwrapper UDP tunnel).

## Delivery modes

The same patch ships three ways; pick one.

| Mode | File | Where it goes | Loaded by |
|------|------|---------------|-----------|
| **dinput proxy** | `dinput.dll` | next to `CARMA2_HW.EXE` | the game (it imports `DINPUT.dll`) |
| **Interposer plugin** | `LANCommander.Interposer.Plugin.Carmageddon2.dll` | `.interposer\Plugins\` | the LANCommander Interposer |
| **ASI** | `carma2patch.asi` | next to `CARMA2_HW.EXE` | an ASI loader (e.g. Ultimate ASI Loader) |

All three run before the game's entry point, verify the exe, read the settings, and install the
same patches. The dinput proxy loads the real `dinput.dll` from the system directory and forwards to
it; the Interposer plugin and the ASI reach DirectInput by hooking the game's import table instead.

### Install (dinput proxy — the simplest)

1. Copy `dinput.dll` and `c2patch.ini` next to `CARMA2_HW.EXE`.
2. For game-controller support, also drop a 32-bit `SDL3.dll` or `SDL2.dll` (from
   [libsdl.org](https://libsdl.org)) next to the exe.
3. Run the game. `c2patch.log` next to the exe records what was applied.

To uninstall, delete the files you copied.

### Install (Interposer plugin)

Put `LANCommander.Interposer.Plugin.Carmageddon2.dll` in `.interposer\Plugins\`. On first run it
registers its defaults into `.interposer\Config.yml` under `Plugins: Carmageddon2:`; edit them there
(or in a `c2patch.ini` next to the plugin DLL, which wins). Log lines are also sent to the
Interposer tagged `[CARMA2]`.

## Settings

Options live in **`c2patch.ini`** (INI sections below), or — as an Interposer plugin — under
`Plugins.Carmageddon2.<Section>.<Key>` in `.interposer\Config.yml`. When both exist, the INI next to
the DLL wins (so the values the in-game menus save there take effect).

| Section | Key | Default | What it does |
|---------|-----|---------|--------------|
| `General` | `Enabled` | `1` | Master switch for the whole patch. |
| `Render` | `MinYonFactor` | `1.0` | Lowest track-column draw factor the graphics menu may set (`0` = don't touch). |
| `Render` | `MinYon` | `0` | Lowest far-clip distance (`0` = don't touch). |
| `Render` | `AnimateAllGrooves` | `1` | Keep moving scenery animating when off-screen. |
| `Net` | `FixCrushListOverflow` | `1` | Fix the 8+ player crush-list overflow crash. |
| `Net` | `SendIntervalMs` | `80` | Car-position send interval in ms (40 = 25 Hz … 120 = 8 Hz). |
| `Net` | `UpdateRateMenu` | `1` | Show the UPDATE RATE row in NET GAME OPTIONS. |
| `Net` | `SkipDuplicateSends` | `1` | Don't resend unchanged car data between simulation steps. |
| `Player` | `MaxNameLength` | `13` | Longest driver name you can type (stock is 8, 13 is the max). |
| `AI` | `OpponentCheating` | `0` | `0` = opponents always use full physics (no slide-along-path cheat). |
| `AI` | `CheatDelayMs` | `3000` | With cheating on, how long out-of-range before it kicks in. |
| `Gamepad` | `Enabled` | `1` | Expose an SDL game controller as a virtual DirectInput joystick. |
| `Gamepad` | `Throttle` | `0` | `0` triggers, `1` left stick, `2` either. |
| `Gamepad` | `StickDeadzone` / `TriggerDeadzone` | `15` / `5` | Dead zones, percent of travel. |
| `Gamepad` | `HideDirectInputJoysticks` | `0` | `1` = hide the same controllers' native DirectInput devices. |
| `Gamepad` | `MenuControls` | `1` | Pad works the menus (D-pad/stick move, A/Start select, B back, Start pause). |
| `Gamepad` | `ButtonActions` | `1` | Pad buttons drive the race with a Max Damage layout (see below). |
| `Gamepad` | `Rumble` | `100` | Rumble strength percent (`0` = off). |
| `Gamepad` | `ImpactScale` | `1.5` | How hard a hit gives full rumble. |

### Controller layout (`ButtonActions`)

Accelerate / brake / steer are the triggers and left stick. Buttons use Carmageddon: Max Damage's
default layout, added alongside the keyboard (it follows any rebind you make in Configure Controls):

B hand brake · X repair · Y recover · A use power-up · LB/RB previous/next power-up · left-stick
click horn · right-stick click map · D-pad ← ↑ → buy Armour / Power / Offense.

See [`docs/gamepad.md`](docs/gamepad.md) for the full mapping and the rumble/no-rumble diagnostics.

## Building

Needs MSYS2 with the 32-bit mingw toolchain (`C:\msys64\mingw32`). From a shell:

```sh
sh build.sh
```

This compiles the shared core once and links all three DLLs into `build/`. `CARMA2_VERSION` (set by
CI from a `vX.Y.Z` tag) is embedded and logged; it defaults to `dev`.

## Repository layout

```
src/                 patch source
  c2patch.c/.h       core: exe check, settings (INI + Config.yml), memory helpers, module install
  dinput_wrap.c      shared DirectInputCreateA wrapper
  dinput_proxy.c     entry point: dinput.dll proxy
  interposer_plugin.c  entry point: LANCommander Interposer plugin (InterposerPluginInit)
  asi.c              entry point: ASI plugin
  render.c ai.c net.c netrate.c names.c gamepad.c   patch modules
  iat.c              import-table hook helper
  nettest*.c         multiplayer test harness (inert unless C2_UNIT is set)
dist/c2patch.ini     default settings (also the source of the Config.yml defaults)
docs/                findings and addresses (draw distance, network, gamepad)
.github/workflows/   CI build + release
```

The research tooling for this patch — the Ghidra project, headless MCP scripts, and the multiplayer
test harness — lives alongside it (`ghidra_scripts/`, `scripts/`, `tools/`) and is documented in
[`CLAUDE.md`](CLAUDE.md).

## License

MIT — see [`LICENSE`](LICENSE).
