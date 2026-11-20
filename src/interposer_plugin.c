/* Delivery mode 2: LANCommander Interposer plugin.
 *
 * Built as LANCommander.Interposer.Plugin.Carmageddon2.dll and dropped in .interposer\Plugins\.
 * The Interposer LoadLibrary's it from its own DllMain (still before the game's entry point) and
 * calls InterposerPluginInit with its own module handle. We apply the patches with Config.yml
 * (Plugins.Carmageddon2) as an extra settings source, and -- since this DLL is not the game's
 * dinput.dll -- hook the game's import table to wrap DirectInput for the gamepad / nettest. */
#include <windows.h>
#include "c2patch.h"

static HINSTANCE g_self;

__declspec(dllexport) void WINAPI InterposerPluginInit(HMODULE interposer)
{
    c2_patch_init(g_self, interposer);
    if (c2_unit || c2_gamepad_enabled()) {
        c2_dinput_install_iat();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;   /* the Interposer loads us dynamically; patching waits for InterposerPluginInit */
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_self = inst;
    }
    return TRUE;
}
