/* Delivery mode 3: ASI plugin.
 *
 * Built as carma2patch.asi and loaded by an ASI loader (e.g. Ultimate ASI Loader, itself usually a
 * dinput.dll/version.dll proxy) during its own DllMain, before the game's entry point. An ASI is a
 * plain DLL with no required exports, so we patch on DLL_PROCESS_ATTACH. Like the Interposer plugin
 * (and unlike the dinput proxy) this DLL is not the game's dinput.dll, so we hook the game's import
 * table to wrap DirectInput for the gamepad / nettest. */
#include <windows.h>
#include "c2patch.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        c2_patch_init(inst, NULL);
        if (c2_unit || c2_gamepad_enabled()) {
            c2_dinput_install_iat();
        }
    }
    return TRUE;
}
