/* Delivery mode 1: dinput.dll proxy.
 *
 * The game imports DINPUT.dll!DirectInputCreateA and none ships in the game folder, so Windows
 * loads this file from there first. We load the real dinput.dll from the system directory and
 * forward the call through the shared wrapper (dinput_wrap.c); DllMain applies the patches before
 * CARMA2_HW.EXE's entry point runs. A static (import) load has a non-NULL lpReserved. */
#include <windows.h>
#include "c2patch.h"

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
    static int set;
    if (!set) {
        FARPROC real = real_proc("DirectInputCreateA");
        if (!real) {
            return E_FAIL;
        }
        c2_dinput_set_real((void *)real);
        set = 1;
    }
    return c2_dinput_create(inst, ver, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        if (reserved) {
            c2_patch_init(inst, NULL);
        }
    }
    return TRUE;
}
