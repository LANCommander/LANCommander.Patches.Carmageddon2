/* The DirectInputCreateA wrapper, shared by all three entry points.
 *
 * The game creates its DirectInput object with DINPUT.dll!DirectInputCreateA; wrapping the returned
 * object is how the gamepad (virtual joystick, menu keys) and the nettest harness get in. Each
 * delivery mode reaches that call differently:
 *  - dinput.dll proxy: our exported DirectInputCreateA forwards here, with the system DLL's export
 *    set as the real target (c2_dinput_set_real).
 *  - Interposer plugin / .asi: our DLL is not the game's dinput.dll, so we hook the game's import
 *    table entry for DINPUT.dll!DirectInputCreateA to c2_dinput_create (c2_dinput_install_iat).
 */
#include "c2patch.h"

typedef HRESULT (WINAPI *DirectInputCreateA_fn)(HINSTANCE, DWORD, void **, void *);
static DirectInputCreateA_fn g_real;

void c2_dinput_set_real(void *direct_input_create_a)
{
    g_real = (DirectInputCreateA_fn)(void (*)(void))direct_input_create_a;
}

HRESULT WINAPI c2_dinput_create(HINSTANCE inst, DWORD ver, void **out, void *outer)
{
    HRESULT hr;
    if (!g_real) {
        return E_FAIL;
    }
    hr = g_real(inst, ver, out, outer);
    c2_log("proxy: DirectInputCreateA ver=%lx hr=%lx di=%p",
           (unsigned long)ver, (unsigned long)hr, (hr == 0 && out) ? *out : 0);
    if (hr == 0 && out) {
        c2_gamepad_wrap_dinput(*out);
        c2_nettest_wrap_dinput(*out);
    }
    return hr;
}

void c2_dinput_install_iat(void)
{
    void *old = c2_hook_import("DINPUT.dll", "DirectInputCreateA", (void *)c2_dinput_create);
    if (old) {
        c2_dinput_set_real(old);
    }
}
