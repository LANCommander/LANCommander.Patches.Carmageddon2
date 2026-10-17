/* dinput.dll proxy: the game imports DINPUT.dll!DirectInputCreateA, so Windows loads this
 * file from the game folder first. The real DLL is loaded from the system directory and the
 * call forwarded; DllMain applies the patches before CARMA2_HW.EXE's entry point runs. */
#include <windows.h>
#include "c2patch.h"

typedef HRESULT (WINAPI *DirectInputCreateA_fn)(HINSTANCE, DWORD, void **, void *);
static DirectInputCreateA_fn real_DirectInputCreateA;

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
    return real_DirectInputCreateA(inst, ver, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        c2_patch_init(inst);
    }
    return TRUE;
}
