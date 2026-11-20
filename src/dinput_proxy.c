/* Entry points. The same DLL loads two ways:
 *
 * - dinput.dll proxy: the game imports DINPUT.dll!DirectInputCreateA, so Windows loads this
 *   file from the game folder first. The real DLL is loaded from the system directory and the
 *   call forwarded; DllMain applies the patches before CARMA2_HW.EXE's entry point runs.
 * - LANCommander Interposer plugin (.interposer\Plugins\c2patch.dll): Interposer LoadLibrary's it
 *   from its own DllMain (still before the game's entry point) and calls InterposerPluginInit,
 *   which applies the patches with Config.yml (Plugins.c2patch) as an extra settings source.
 *
 * DllMain tells them apart by lpReserved, which is non-NULL only for static (import) loads. */
#include <windows.h>
#include "c2patch.h"

typedef HRESULT (WINAPI *DirectInputCreateA_fn)(HINSTANCE, DWORD, void **, void *);
static DirectInputCreateA_fn real_DirectInputCreateA;
static HINSTANCE g_self;

static FARPROC real_proc(const char *name)
{
    static HMODULE real;
    if (!real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);   /* SysWOW64 for a 32-bit process */
        lstrcpynA(path + n, "\\dinput.dll", MAX_PATH - n);
        real = LoadLibraryA(path);
        if (!real) {
            c2_log("cannot load %s (%lu)", path, GetLastError());
            return NULL;
        }
    }
    return GetProcAddress(real, name);
}

__declspec(dllexport) HRESULT WINAPI DirectInputCreateA(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    if (!real_DirectInputCreateA) {
        real_DirectInputCreateA = (DirectInputCreateA_fn)(void (*)(void))real_proc("DirectInputCreateA");
        if (!real_DirectInputCreateA) {
            return E_FAIL;
        }
    }
    {
        HRESULT hr = real_DirectInputCreateA(inst, ver, out, outer);
        c2_log("proxy: DirectInputCreateA ver=%lx hr=%lx di=%p", (unsigned long)ver, (unsigned long)hr, (hr == 0 && out) ? *out : 0);
        if (hr == 0 && out) {
            c2_gamepad_wrap_dinput(*out);
            c2_nettest_wrap_dinput(*out);
        }
        return hr;
    }
}

__declspec(dllexport) void WINAPI InterposerPluginInit(HMODULE interposer)
{
    c2_patch_init(g_self, interposer);
    if (c2_unit || c2_gamepad_enabled()) {
        /* not the game's dinput.dll here: reach the DirectInput wrappers through the IAT */
        real_DirectInputCreateA = (DirectInputCreateA_fn)(void (*)(void))
            c2_hook_import("DINPUT.dll", "DirectInputCreateA", (void *)DirectInputCreateA);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_self = inst;
        if (reserved) {
            c2_patch_init(inst, NULL);
        }
    }
    return TRUE;
}
