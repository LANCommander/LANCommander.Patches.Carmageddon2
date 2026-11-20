/* Game controllers through SDL.
 *
 * The game reads joysticks with DirectInput 5 (platform code around 0x459000): EnumDevices
 * (DIDEVTYPE_JOYSTICK) at startup, c_dfDIJoystick, and per frame Poll + GetDeviceState for
 * lX (steering, 0x8000 = centre), lY (below 0x8000 accelerates, above brakes/reverses) and
 * rgbButtons[0..31], which become keys 107..138 ("Joy 1 B1".. in KEYNAMES.TXT) for the controls
 * menu. gJoystick_index (0x595f88) picks the device, and it starts at the first one enumerated.
 *
 * Instead of patching that, the IDirectInputA the game gets lists one more joystick (first if a
 * pad is connected, see hk_EnumDevices): a virtual IDirectInputDevice2A whose state comes from the first SDL gamepad (SDL3.dll, else
 * SDL2.dll, loaded at run time). SDL gives every pad the same layout, triggers as separate axes,
 * and hotplugging (the virtual device stays, the pad behind it can come and go). It reports no
 * force feedback, so the game's I-Force code (FFB.IFR) stays off; rumble comes from hooks on the
 * game's force-feedback calls instead (see below).
 *
 * Menus only read the keyboard, so while a menu runs the pad also presses arrow keys, Return
 * and Escape (added to the keyboard's GetDeviceState). "A menu runs": not racing
 * (gProgram_state.racing, 0x75bba8), or inside a FrontEndProcessMenus (0x46d8e0) entered during a
 * race (the pause menu). Not while Controls_SlotActivated (0x472b00) waits for a key to bind,
 * so a pad button binds as itself rather than as Return.
 *
 * [Gamepad] Enabled, Throttle, StickDeadzone, TriggerDeadzone, HideDirectInputJoysticks, MenuControls,
 * Rumble, ImpactScale.
 */
#define DIRECTINPUT_VERSION 0x0500
#include <dinput.h>
#include <math.h>
#include <string.h>
#include "c2patch.h"

#define RACING (*(volatile int *)0x75bba8)   /* gProgram_state.racing */

/* SDL2 and SDL3 number gamepad buttons and axes the same way */
enum { PAD_A, PAD_B, PAD_X, PAD_Y, PAD_BACK, PAD_GUIDE, PAD_START, PAD_LSTICK, PAD_RSTICK, PAD_LB, PAD_RB,
       PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_SDL_BUTTONS = 21 };
enum { AX_LX, AX_LY, AX_RX, AX_RY, AX_LT, AX_RT };
#define PAD_BUTTONS (PAD_SDL_BUTTONS + 2)    /* + LT and RT as buttons 22 and 23 */
#define PAD_AXES 6                           /* X Y Z Rx Ry Rz */

/* the few SDL functions used (SDLCALL is __cdecl); the two versions differ in names and in
   bool (SDL3, one byte) vs int (SDL2) results */
static struct {
    int version;
    void (*free)(void *);
    void (*update)(void);
    void (*flush)(unsigned lo, unsigned hi);
    const char *(*error)(void);
    void *(*open)(unsigned id);          /* SDL2: device index, SDL3: joystick id */
    void (*close)(void *pad);
    const char *(*name)(void *pad);
    short (*axis)(void *pad, int axis);
    unsigned char (*button)(void *pad, int button);
    int (*sdl2_attached)(void *pad);
    unsigned char (*sdl3_connected)(void *pad);
    int (*sdl2_count)(void);
    int (*sdl2_is_pad)(int index);
    unsigned *(*sdl3_pads)(int *count);
    int (*rumble)(void *pad, unsigned short low, unsigned short high, unsigned ms);   /* optional; SDL2 0=ok, SDL3 true=ok */
    unsigned char (*sdl3_has_rumble)(void *pad);
    unsigned char (*sdl2_has_rumble)(void *pad);
} sdl;

static int g_enabled, g_throttle, g_hide_dinput, g_menu_controls, g_button_actions;
static float g_stick_dz, g_trigger_dz;
static int g_sdl_state;                  /* 0 not tried, 1 ready, -1 unavailable */
static void *g_pad;
static char g_pad_name[64];
static DWORD g_last_scan;
static LARGE_INTEGER g_last_update, g_qpf;

/* the latest pad state, already in DirectInput terms */
static float g_axis[PAD_AXES];           /* -1..1 (X, Y as the game wants them; triggers 0..1) */
static unsigned char g_buttons[PAD_BUTTONS];
static DWORD g_pov = 0xffffffff;
static float g_lx, g_ly;                 /* for the menu arrows */

static int g_menu_depth, g_menu_racing[16], g_binding;

/* guidInstance and guidProduct of the virtual device */
static const GUID g_pad_guid = {0xc2ad5d10, 0x5d1f, 0x4c2a, {0x9a, 0x33, 0x63, 0x61, 0x72, 0x6d, 0x61, 0x32}};

static void *sym(HMODULE dll, const char *name)
{
    void *p = (void *)GetProcAddress(dll, name);
    if (!p) {
        c2_log("gamepad: %s missing", name);
    }
    return p;
}

/* nettest (C2_UNIT with C2_PAD=1, SDL2 only): a virtual SDL gamepad, set by "pad" commands
   (c2_gamepad_test). SDL maps a virtual gamepad's buttons and axes in SDL's own order. */
static void *g_test_joy;
static int g_test_index = -1;            /* device index of the virtual pad; open_first_pad prefers it */
static int (*sdl2_set_axis)(void *joy, int axis, short value);
static int (*sdl2_set_button)(void *joy, int button, unsigned char value);

static void test_pad_attach(HMODULE dll)
{
    int (*attach)(int type, int naxes, int nbuttons, int nhats) = (void *)GetProcAddress(dll, "SDL_JoystickAttachVirtual");
    void *(*open)(int index) = (void *)GetProcAddress(dll, "SDL_JoystickOpen");
    int index;
    *(void **)&sdl2_set_axis = (void *)GetProcAddress(dll, "SDL_JoystickSetVirtualAxis");
    *(void **)&sdl2_set_button = (void *)GetProcAddress(dll, "SDL_JoystickSetVirtualButton");
    if (!attach || !open || !sdl2_set_axis || !sdl2_set_button) {
        return;
    }
    index = attach(1, 6, PAD_SDL_BUTTONS, 0);     /* SDL_JOYSTICK_TYPE_GAMECONTROLLER */
    g_test_joy = index >= 0 ? open(index) : NULL;
    g_test_index = g_test_joy ? index : -1;       /* so the test opens this, not a real pad on the dev box */
    c2_log("gamepad: test pad %s", g_test_joy ? "attached" : sdl.error());
}

int c2_gamepad_test(int is_axis, int index, int value)
{
    if (!g_test_joy) {
        return 0;
    }
    if (is_axis) {
        sdl2_set_axis(g_test_joy, index, (short)(value > 32767 ? 32767 : value < -32768 ? -32768 : value));
    } else {
        sdl2_set_button(g_test_joy, index, (unsigned char)(value != 0));
    }
    return 1;
}

int c2_gamepad_is_virtual(const void *guid)
{
    return IsEqualGUID(guid, &g_pad_guid);
}

static int sdl_load(void)
{
    typedef int (*init2_fn)(unsigned);
    typedef unsigned char (*init3_fn)(unsigned);
    typedef void (*hint_fn)(const char *, const char *);
    static const char *const names[] = {"SDL3.dll", "SDL2.dll"};
    HMODULE dll = NULL;
    hint_fn hint;
    int i, ok;

    for (i = 0; i < 2 && !dll; i++) {
        dll = LoadLibraryA(names[i]);
        sdl.version = 3 - i;
    }
    if (!dll) {
        c2_log("gamepad: no SDL3.dll or SDL2.dll next to the game; SDL gamepads off");
        return 0;
    }
#define S(field, name) (*(void **)&sdl.field = sym(dll, name)) != NULL
    hint = (hint_fn)(void (*)(void))GetProcAddress(dll, "SDL_SetHint");
    if (sdl.version == 3) {
        ok = S(free, "SDL_free") && S(update, "SDL_UpdateGamepads") && S(flush, "SDL_FlushEvents")
            && S(error, "SDL_GetError") && S(open, "SDL_OpenGamepad") && S(close, "SDL_CloseGamepad")
            && S(name, "SDL_GetGamepadName") && S(axis, "SDL_GetGamepadAxis")
            && S(button, "SDL_GetGamepadButton") && S(sdl3_connected, "SDL_GamepadConnected")
            && S(sdl3_pads, "SDL_GetGamepads");
    } else {
        ok = S(update, "SDL_GameControllerUpdate") && S(flush, "SDL_FlushEvents")
            && S(error, "SDL_GetError") && S(open, "SDL_GameControllerOpen")
            && S(close, "SDL_GameControllerClose") && S(name, "SDL_GameControllerName")
            && S(axis, "SDL_GameControllerGetAxis") && S(button, "SDL_GameControllerGetButton")
            && S(sdl2_attached, "SDL_GameControllerGetAttached") && S(sdl2_count, "SDL_NumJoysticks")
            && S(sdl2_is_pad, "SDL_IsGameController");
    }
#undef S
    *(void **)&sdl.rumble = (void *)GetProcAddress(dll, sdl.version == 3 ? "SDL_RumbleGamepad" : "SDL_GameControllerRumble");
    *(void **)&sdl.sdl3_has_rumble = (void *)GetProcAddress(dll, "SDL_GamepadHasRumble");
    *(void **)&sdl.sdl2_has_rumble = (void *)GetProcAddress(dll, "SDL_GameControllerHasRumble");
    c2_log("gamepad: rumble %s", sdl.rumble ? "available" : "unavailable (SDL too old; need SDL2 >= 2.0.9 or SDL3)");
    if (!ok || !hint) {
        return 0;
    }
    /* the game window has focus anyway, and DirectInput joysticks were read in the background too;
       raw input messages get their own thread (nothing pumps SDL's helper window otherwise) */
    hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    hint("SDL_JOYSTICK_THREAD", "1");
    /* Embedded in a non-SDL game, SDL doesn't own a window pumping WM_INPUT, so its RAWINPUT
       backend never correlates the pad with XInput: axes/buttons read 0 and rumble fails with
       "isn't correlated yet". Poll XInput directly instead (Xbox-class pads, 8BitDo in X mode). */
    hint("SDL_JOYSTICK_RAWINPUT", "0");
    if (sdl.version == 3) {
        init3_fn init = (init3_fn)(void (*)(void))sym(dll, "SDL_Init");
        ok = init && init(0x2000);            /* SDL_INIT_GAMEPAD */
    } else {
        init2_fn init = (init2_fn)(void (*)(void))sym(dll, "SDL_Init");
        ok = init && init(0x2000) == 0;       /* SDL_INIT_GAMECONTROLLER */
    }
    if (!ok) {
        c2_log("gamepad: SDL_Init failed: %s", sdl.error());
        return 0;
    }
    c2_log("gamepad: SDL%d loaded", sdl.version);
    if (c2_unit && sdl.version == 2 && GetEnvironmentVariableA("C2_PAD", NULL, 0)) {
        test_pad_attach(dll);
    }
    return 1;
}

static int sdl_ready(void)
{
    if (!g_sdl_state) {
        g_sdl_state = sdl_load() ? 1 : -1;
        QueryPerformanceFrequency(&g_qpf);
    }
    return g_sdl_state > 0;
}

static void *open_first_pad(void)
{
    void *pad = NULL;
    int i, n = 0;
    if (sdl.version == 3) {
        unsigned *ids = sdl.sdl3_pads(&n);
        for (i = 0; ids && i < n && !pad; i++) {
            pad = sdl.open(ids[i]);
        }
        sdl.free(ids);
    } else if (g_test_index >= 0) {
        pad = sdl.open((unsigned)g_test_index);   /* harness: the virtual pad, not a real one plugged in */
    } else {
        n = sdl.sdl2_count();
        for (i = 0; i < n && !pad; i++) {
            if (sdl.sdl2_is_pad(i)) {
                pad = sdl.open((unsigned)i);
            }
        }
    }
    return pad;
}

static int pad_connected(void *pad)
{
    return sdl.version == 3 ? sdl.sdl3_connected(pad) != 0 : sdl.sdl2_attached(pad) != 0;
}

static float dead(float v, float dz)
{
    float a = fabsf(v);
    if (a <= dz) {
        return 0.0f;
    }
    a = (a - dz) / (1.0f - dz);
    a = a > 1.0f ? 1.0f : a;
    return v < 0 ? -a : a;
}

static float axis(int a)
{
    float v = sdl.axis(g_pad, a) / 32767.0f;
    return v < -1.0f ? -1.0f : v;
}

static void rumble_tick(void);
static void rumble_connect_test(void);

/* reads the pad (at most every 2 ms: the game polls the joystick several times a frame) */
static void pad_update(void)
{
    LARGE_INTEGER now;
    float lt, rt, acc = 0, brk = 0;
    int i;

    if (!sdl_ready()) {
        return;
    }
    QueryPerformanceCounter(&now);
    if (now.QuadPart - g_last_update.QuadPart < g_qpf.QuadPart / 500) {
        return;
    }
    g_last_update = now;
    sdl.update();
    sdl.flush(0, 0xffffffffu);           /* nobody reads SDL's event queue */
    if (g_pad && !pad_connected(g_pad)) {
        c2_log("gamepad: %s disconnected", g_pad_name);
        sdl.close(g_pad);
        g_pad = NULL;
    }
    if (!g_pad && GetTickCount() - g_last_scan > 1000) {
        g_last_scan = GetTickCount();
        if ((g_pad = open_first_pad()) != NULL) {
            const char *name = sdl.name(g_pad);
            lstrcpynA(g_pad_name, name ? name : "gamepad", sizeof g_pad_name);
            c2_log("gamepad: using %s", g_pad_name);
            rumble_connect_test();
        }
    }
    rumble_tick();
    memset(g_axis, 0, sizeof g_axis);
    memset(g_buttons, 0, sizeof g_buttons);
    g_pov = 0xffffffff;
    g_lx = g_ly = 0;
    if (!g_pad) {
        return;
    }
    g_lx = axis(AX_LX);
    g_ly = axis(AX_LY);
    lt = dead(axis(AX_LT), g_trigger_dz);
    rt = dead(axis(AX_RT), g_trigger_dz);
    if (g_throttle != 1) {
        acc = rt;
        brk = lt;
    }
    if (g_throttle != 0) {
        float y = dead(g_ly, g_stick_dz);
        acc = y < -acc ? -y : acc;
        brk = y > brk ? y : brk;
    }
    g_axis[0] = dead(g_lx, g_stick_dz);
    g_axis[1] = brk - acc;
    g_axis[2] = lt;
    g_axis[3] = dead(axis(AX_RX), g_stick_dz);
    g_axis[4] = dead(axis(AX_RY), g_stick_dz);
    g_axis[5] = rt;
    for (i = 0; i < PAD_SDL_BUTTONS; i++) {
        g_buttons[i] = sdl.button(g_pad, i) ? 0x80 : 0;
    }
    g_buttons[PAD_SDL_BUTTONS] = lt > 0.5f ? 0x80 : 0;
    g_buttons[PAD_SDL_BUTTONS + 1] = rt > 0.5f ? 0x80 : 0;
    {
        int x = !!g_buttons[PAD_RIGHT] - !!g_buttons[PAD_LEFT];
        int y = !!g_buttons[PAD_DOWN] - !!g_buttons[PAD_UP];
        static const DWORD hat[3][3] = {{31500, 0, 4500}, {27000, 0xffffffff, 9000}, {22500, 18000, 13500}};
        g_pov = hat[y + 1][x + 1];
    }
}

/* ---- the virtual DirectInput device ---- */

static IDirectInputDevice2A g_dev;
static LONG g_range[PAD_AXES][2];
static int g_dev_polled;                 /* diagnostics: has the game read our joystick yet */

static HRESULT WINAPI dev_QueryInterface(IDirectInputDevice2A *self, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IDirectInputDevice2A) || IsEqualGUID(iid, &IID_IDirectInputDeviceA)
        || IsEqualGUID(iid, &IID_IUnknown)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* one static object: reference counts don't matter */
static ULONG WINAPI dev_AddRef(IDirectInputDevice2A *self) { (void)self; return 1; }
static ULONG WINAPI dev_Release(IDirectInputDevice2A *self) { (void)self; return 1; }

static void fill_instance(DIDEVICEINSTANCEA *di)
{
    DWORD size = di->dwSize ? di->dwSize : sizeof *di;
    memset(di, 0, size);
    di->dwSize = size;
    di->guidInstance = g_pad_guid;
    di->guidProduct = g_pad_guid;
    di->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8) | DIDEVTYPE_HID;
    wsprintfA(di->tszInstanceName, "SDL gamepad");
    if (g_pad) {
        wsprintfA(di->tszProductName, "SDL: %s", g_pad_name);
    } else {
        lstrcpyA(di->tszProductName, "SDL gamepad");
    }
}

static HRESULT WINAPI dev_GetCapabilities(IDirectInputDevice2A *self, DIDEVCAPS *caps)
{
    DWORD size = caps->dwSize;
    (void)self;
    memset(caps, 0, size);
    caps->dwSize = size;
    caps->dwFlags = DIDC_ATTACHED;
    caps->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8) | DIDEVTYPE_HID;
    caps->dwAxes = PAD_AXES;
    caps->dwButtons = PAD_BUTTONS;
    caps->dwPOVs = 1;
    return DI_OK;
}

/* objects by DIJOYSTATE offset: axes 0..0x14, POV 0x20, buttons 0x30.. */
static int object_info(DWORD ofs, DIDEVICEOBJECTINSTANCEA *oi)
{
    static const GUID *const axis_guid[PAD_AXES] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis};
    static const char *const axis_name[PAD_AXES] = {"Left stick X", "Throttle/brake", "Left trigger", "Right stick X", "Right stick Y", "Right trigger"};
    DWORD size = oi->dwSize;

    memset(oi, 0, size);
    oi->dwSize = size;
    oi->dwOfs = ofs;
    if (ofs < 4 * PAD_AXES && ofs % 4 == 0) {
        oi->guidType = *axis_guid[ofs / 4];
        oi->dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(ofs / 4);
        lstrcpyA(oi->tszName, axis_name[ofs / 4]);
    } else if (ofs == DIJOFS_POV(0)) {
        oi->guidType = GUID_POV;
        oi->dwType = DIDFT_POV | DIDFT_MAKEINSTANCE(0);
        lstrcpyA(oi->tszName, "D-pad");
    } else if (ofs >= DIJOFS_BUTTON0 && ofs < DIJOFS_BUTTON(PAD_BUTTONS)) {
        oi->guidType = GUID_Button;
        oi->dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(ofs - DIJOFS_BUTTON0);
        wsprintfA(oi->tszName, "Button %lu", (unsigned long)(ofs - DIJOFS_BUTTON0 + 1));
    } else {
        return 0;
    }
    return 1;
}

static HRESULT WINAPI dev_EnumObjects(IDirectInputDevice2A *self, LPDIENUMDEVICEOBJECTSCALLBACKA cb, void *ref, DWORD flags)
{
    DIDEVICEOBJECTINSTANCEA oi;
    DWORD ofs;
    (void)self;
    for (ofs = 0; ofs < DIJOFS_BUTTON(PAD_BUTTONS); ofs++) {
        oi.dwSize = sizeof oi;
        if (!object_info(ofs, &oi) || (flags != DIDFT_ALL && !(DIDFT_GETTYPE(oi.dwType) & flags))) {
            continue;
        }
        if (cb(&oi, ref) == DIENUM_STOP) {
            break;
        }
    }
    return DI_OK;
}

static HRESULT WINAPI dev_GetObjectInfo(IDirectInputDevice2A *self, DIDEVICEOBJECTINSTANCEA *oi, DWORD obj, DWORD how)
{
    (void)self;
    return how == DIPH_BYOFFSET && object_info(obj, oi) ? DI_OK : DIERR_OBJECTNOTFOUND;
}

/* DIPROP_RANGE is the only property that changes anything; the rest are accepted (the game
   gives up on a device whose SetProperty fails) */
static HRESULT WINAPI dev_SetProperty(IDirectInputDevice2A *self, REFGUID prop, const DIPROPHEADER *ph)
{
    (void)self;
    if (prop == DIPROP_RANGE && ph->dwSize >= sizeof(DIPROPRANGE)) {
        const DIPROPRANGE *r = (const DIPROPRANGE *)ph;
        int i;
        for (i = 0; i < PAD_AXES; i++) {
            if (ph->dwHow == DIPH_DEVICE || (ph->dwHow == DIPH_BYOFFSET && ph->dwObj == 4u * i)) {
                g_range[i][0] = r->lMin;
                g_range[i][1] = r->lMax;
            }
        }
    }
    return DI_OK;
}

static HRESULT WINAPI dev_GetProperty(IDirectInputDevice2A *self, REFGUID prop, DIPROPHEADER *ph)
{
    (void)self;
    if (prop == DIPROP_RANGE && ph->dwSize >= sizeof(DIPROPRANGE) && ph->dwHow == DIPH_BYOFFSET
        && ph->dwObj < 4 * PAD_AXES && ph->dwObj % 4 == 0) {
        ((DIPROPRANGE *)ph)->lMin = g_range[ph->dwObj / 4][0];
        ((DIPROPRANGE *)ph)->lMax = g_range[ph->dwObj / 4][1];
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_ok(IDirectInputDevice2A *self) { (void)self; return DI_OK; }

static LONG to_range(int a, float v)    /* -1..1, 0 = the centre of the range */
{
    double lo = g_range[a][0], hi = g_range[a][1], mid = floor((lo + hi + 1) / 2);
    return (LONG)floor(v < 0 ? mid + v * (mid - lo) : mid + v * (hi - mid) + 0.5);
}

static HRESULT WINAPI dev_GetDeviceState(IDirectInputDevice2A *self, DWORD size, void *data)
{
    DIJOYSTATE *js = data;
    (void)self;
    if (size < sizeof(DIJOYSTATE)) {
        return DIERR_INVALIDPARAM;
    }
    pad_update();
    if (!g_dev_polled) {
        g_dev_polled = 1;
        c2_log("gamepad: game is reading the virtual pad (joystick input is live)");
    }
    memset(data, 0, size);
    js->lX = to_range(0, g_axis[0]);
    js->lY = to_range(1, g_axis[1]);
    js->lZ = to_range(2, g_axis[2] * 2 - 1);
    js->lRx = to_range(3, g_axis[3]);
    js->lRy = to_range(4, g_axis[4]);
    js->lRz = to_range(5, g_axis[5] * 2 - 1);
    js->rgdwPOV[0] = g_pov;
    js->rgdwPOV[1] = js->rgdwPOV[2] = js->rgdwPOV[3] = 0xffffffff;
    memcpy(js->rgbButtons, g_buttons, PAD_BUTTONS);
    return DI_OK;
}

static HRESULT WINAPI dev_GetDeviceData(IDirectInputDevice2A *self, DWORD cb, DIDEVICEOBJECTDATA *od, DWORD *inout, DWORD flags)
{
    (void)self; (void)cb; (void)od; (void)flags;
    *inout = 0;                          /* no buffered data */
    return DI_OK;
}

static HRESULT WINAPI dev_SetDataFormat(IDirectInputDevice2A *self, const DIDATAFORMAT *df)
{
    (void)self;
    return df->dwDataSize >= sizeof(DIJOYSTATE) ? DI_OK : DIERR_INVALIDPARAM;
}

static HRESULT WINAPI dev_SetEventNotification(IDirectInputDevice2A *self, HANDLE ev) { (void)self; (void)ev; return DI_OK; }
static HRESULT WINAPI dev_SetCooperativeLevel(IDirectInputDevice2A *self, HWND hwnd, DWORD flags) { (void)self; (void)hwnd; (void)flags; return DI_OK; }

static HRESULT WINAPI dev_GetDeviceInfo(IDirectInputDevice2A *self, DIDEVICEINSTANCEA *di)
{
    (void)self;
    fill_instance(di);
    return DI_OK;
}

static HRESULT WINAPI dev_RunControlPanel(IDirectInputDevice2A *self, HWND hwnd, DWORD flags) { (void)self; (void)hwnd; (void)flags; return DI_OK; }
static HRESULT WINAPI dev_Initialize(IDirectInputDevice2A *self, HINSTANCE inst, DWORD ver, REFGUID guid) { (void)self; (void)inst; (void)ver; (void)guid; return DI_OK; }

static HRESULT WINAPI dev_CreateEffect(IDirectInputDevice2A *self, REFGUID guid, LPCDIEFFECT eff, LPDIRECTINPUTEFFECT *out, LPUNKNOWN outer)
{
    (void)self; (void)guid; (void)eff; (void)outer;
    *out = NULL;
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI dev_EnumEffects(IDirectInputDevice2A *self, LPDIENUMEFFECTSCALLBACKA cb, void *ref, DWORD type) { (void)self; (void)cb; (void)ref; (void)type; return DI_OK; }
static HRESULT WINAPI dev_GetEffectInfo(IDirectInputDevice2A *self, LPDIEFFECTINFOA info, REFGUID guid) { (void)self; (void)info; (void)guid; return DIERR_DEVICENOTREG; }
static HRESULT WINAPI dev_GetForceFeedbackState(IDirectInputDevice2A *self, DWORD *out) { (void)self; (void)out; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI dev_SendForceFeedbackCommand(IDirectInputDevice2A *self, DWORD flags) { (void)self; (void)flags; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI dev_EnumCreatedEffectObjects(IDirectInputDevice2A *self, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, void *ref, DWORD fl) { (void)self; (void)cb; (void)ref; (void)fl; return DI_OK; }
static HRESULT WINAPI dev_Escape(IDirectInputDevice2A *self, LPDIEFFESCAPE esc) { (void)self; (void)esc; return DIERR_UNSUPPORTED; }

static HRESULT WINAPI dev_Poll(IDirectInputDevice2A *self)
{
    (void)self;
    pad_update();
    return DI_OK;                        /* the game treats anything else as a failure */
}

static HRESULT WINAPI dev_SendDeviceData(IDirectInputDevice2A *self, DWORD cb, LPCDIDEVICEOBJECTDATA od, DWORD *inout, DWORD fl)
{
    (void)self; (void)cb; (void)od; (void)fl;
    *inout = 0;
    return DI_OK;
}

static IDirectInputDevice2AVtbl g_dev_vtbl = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCapabilities, dev_EnumObjects,
    dev_GetProperty, dev_SetProperty, dev_ok, dev_ok, dev_GetDeviceState, dev_GetDeviceData,
    dev_SetDataFormat, dev_SetEventNotification, dev_SetCooperativeLevel, dev_GetObjectInfo,
    dev_GetDeviceInfo, dev_RunControlPanel, dev_Initialize, dev_CreateEffect, dev_EnumEffects,
    dev_GetEffectInfo, dev_GetForceFeedbackState, dev_SendForceFeedbackCommand,
    dev_EnumCreatedEffectObjects, dev_Escape, dev_Poll, dev_SendDeviceData,
};

/* ---- IDirectInputA and keyboard hooks (the vtables are shared by every object of a kind) ---- */

typedef HRESULT (WINAPI *CreateDevice_fn)(IDirectInputA *, REFGUID, IDirectInputDeviceA **, IUnknown *);
typedef HRESULT (WINAPI *EnumDevices_fn)(IDirectInputA *, DWORD, LPDIENUMDEVICESCALLBACKA, void *, DWORD);
typedef HRESULT (WINAPI *GetDeviceState_fn)(IDirectInputDeviceA *, DWORD, void *);
static CreateDevice_fn real_CreateDevice;
static EnumDevices_fn real_EnumDevices;
static GetDeviceState_fn real_GetDeviceState;
static LPDIENUMDEVICESCALLBACKA g_enum_cb;
static int g_menu_inject_logged;         /* diagnostics: has a pad press reached the menu yet */

static int menu_active(void)
{
    if (g_binding) {
        return 0;
    }
    if (!RACING) {
        return 1;
    }
    return g_menu_depth > 0 && g_menu_racing[(g_menu_depth > 16 ? 16 : g_menu_depth) - 1];
}

/* In-race button actions, the Max Damage layout. A pad button fires a game action by ORing that
   action's current keyboard scancode into the DirectInput keyboard buffer the game reads: it adds
   to the keyboard binding instead of replacing it and follows any rebind in Configure Controls.
   Every action (driving, the gToggle_array toggles, the APO buys) is reached through gKey_mapping:
   gKey_array[gKey_mapping[slot]] comes from diKeys[gScan_code[gKey_mapping[slot] + 12]]. */
#define GKEY_MAPPING ((const int *)0x0074b5e0)   /* int[77]: action slot -> gKey_array index */
#define GSCAN_CODE   ((const int *)0x006b3470)   /* int[256]: gKey_array index -> DIK scancode */

static const struct { unsigned char button, slot; } g_race_actions[] = {
    {PAD_B,      54},   /* B: hand brake */
    {PAD_X,      45},   /* X: repair */
    {PAD_Y,      60},   /* Y: recover */
    {PAD_A,      62},   /* A: open / activate the power-up inventory */
    {PAD_LB,     63},   /* LB: previous power-up */
    {PAD_RB,     64},   /* RB: next power-up */
    {PAD_LSTICK, 59},   /* left stick click: horn */
    {PAD_RSTICK, 57},   /* right stick click: map */
    {PAD_LEFT,   67},   /* D-pad left: buy Armour */
    {PAD_UP,     68},   /* D-pad up: buy Power */
    {PAD_RIGHT,  69},   /* D-pad right: buy Offense */
};

static void inject_action(unsigned char *keys, int slot)
{
    int v = GKEY_MAPPING[slot];
    int scan;
    if (v < 0 || v >= 151) {          /* 107+ are joystick buttons, 143+ joystick axes: no scancode */
        return;
    }
    scan = GSCAN_CODE[v + 12];
    if (scan > 0 && scan < 256) {
        keys[scan] |= 0x80;
    }
}

static HRESULT WINAPI hk_GetDeviceState(IDirectInputDeviceA *dev, DWORD size, void *data)
{
    HRESULT hr = real_GetDeviceState(dev, size, data);
    if (size == 256 && hr == DI_OK && (g_menu_controls || g_button_actions)) {
        unsigned char *k = data;
        pad_update();
        if (!g_pad) {
            return hr;
        }
        if (menu_active()) {
            if (!g_menu_controls) {
                return hr;
            }
            if ((g_buttons[PAD_UP] || g_buttons[PAD_DOWN] || g_buttons[PAD_LEFT] || g_buttons[PAD_RIGHT]
                 || g_buttons[PAD_A] || g_buttons[PAD_B] || g_buttons[PAD_START]
                 || g_lx < -0.5f || g_lx > 0.5f || g_ly < -0.5f || g_ly > 0.5f) && !g_menu_inject_logged) {
                g_menu_inject_logged = 1;
                c2_log("gamepad: menu key injected from the pad (menu controls are live)");
            }
            k[DIK_UP] |= (g_buttons[PAD_UP] || g_ly < -0.5f) ? 0x80 : 0;
            k[DIK_DOWN] |= (g_buttons[PAD_DOWN] || g_ly > 0.5f) ? 0x80 : 0;
            k[DIK_LEFT] |= (g_buttons[PAD_LEFT] || g_lx < -0.5f) ? 0x80 : 0;
            k[DIK_RIGHT] |= (g_buttons[PAD_RIGHT] || g_lx > 0.5f) ? 0x80 : 0;
            k[DIK_RETURN] |= g_buttons[PAD_A] | g_buttons[PAD_START];
            k[DIK_ESCAPE] |= g_buttons[PAD_B];
        } else if (!g_binding) {
            if (g_menu_controls) {
                k[DIK_ESCAPE] |= g_buttons[PAD_START];   /* racing: pause menu */
            }
            if (g_button_actions) {
                size_t j;
                for (j = 0; j < sizeof g_race_actions / sizeof g_race_actions[0]; j++) {
                    if (g_buttons[g_race_actions[j].button]) {
                        inject_action(k, g_race_actions[j].slot);
                    }
                }
            }
        }
    }
    return hr;
}

static HRESULT WINAPI hk_CreateDevice(IDirectInputA *di, REFGUID guid, IDirectInputDeviceA **out, IUnknown *outer)
{
    HRESULT hr;
    if (IsEqualGUID(guid, &g_pad_guid)) {
        *out = (IDirectInputDeviceA *)&g_dev;
        return DI_OK;
    }
    hr = real_CreateDevice(di, guid, out, outer);
    if (hr == DI_OK && *out && (g_menu_controls || g_button_actions) && !real_GetDeviceState) {
        void **vtbl = *(void ***)*out;
        void *hook = (void *)hk_GetDeviceState;
        real_GetDeviceState = (GetDeviceState_fn)vtbl[9];
        c2_write((uint32_t)(uintptr_t)&vtbl[9], &hook, 4);
        c2_log("gamepad: menu-key hook installed");
    }
    return hr;
}

static BOOL g_enum_result;

static BOOL CALLBACK enum_filter(LPCDIDEVICEINSTANCEA inst, void *ref)
{
    if (g_hide_dinput && GET_DIDEVICE_TYPE(inst->dwDevType) == DIDEVTYPE_JOYSTICK) {
        return DIENUM_CONTINUE;
    }
    return g_enum_result = g_enum_cb(inst, ref);
}

/* The game starts with the first joystick listed. With an SDL pad connected that is the SDL
   device; without one it goes last, so a wheel or stick SDL doesn't see as a gamepad stays first
   (a pad plugged in later then needs CycleJoystick, or a restart). */
static HRESULT WINAPI hk_EnumDevices(IDirectInputA *di, DWORD type, LPDIENUMDEVICESCALLBACKA cb, void *ref, DWORD flags)
{
    if ((type == 0 || type == DIDEVTYPE_JOYSTICK) && sdl_ready()) {
        DIDEVICEINSTANCEA inst;
        HRESULT hr = DI_OK;
        pad_update();
        inst.dwSize = sizeof inst;
        fill_instance(&inst);
        c2_log("gamepad: listed to the game as \"%s\" (%s)%s", inst.tszProductName, g_pad ? "first" : "last",
               g_hide_dinput ? ", DirectInput joysticks hidden" : "");
        if (g_pad && cb(&inst, ref) == DIENUM_STOP) {
            return DI_OK;
        }
        g_enum_cb = cb;
        g_enum_result = DIENUM_CONTINUE;
        hr = real_EnumDevices(di, type, enum_filter, ref, flags);
        if (!g_pad && g_enum_result != DIENUM_STOP) {
            cb(&inst, ref);
        }
        return hr;
    }
    return real_EnumDevices(di, type, cb, ref, flags);
}

void c2_gamepad_wrap_dinput(void *di)
{
    static int done;
    void **vtbl;
    void *create = (void *)hk_CreateDevice, *enumdev = (void *)hk_EnumDevices;
    if (!g_enabled || !di || done) {
        return;
    }
    done = 1;
    vtbl = *(void ***)di;
    real_CreateDevice = (CreateDevice_fn)vtbl[3];
    real_EnumDevices = (EnumDevices_fn)vtbl[4];
    c2_write((uint32_t)(uintptr_t)&vtbl[3], &create, 4);
    c2_write((uint32_t)(uintptr_t)&vtbl[4], &enumdev, 4);
}

/* ---- menu tracking ---- */

typedef int (__attribute__((fastcall)) *fastcall1_fn)(int);
static fastcall1_fn g_menus_orig, g_slot_orig;

static int __attribute__((fastcall)) hk_FrontEndProcessMenus(int type)
{
    int r;
    if (g_menu_depth < 16) {
        g_menu_racing[g_menu_depth] = RACING;
    }
    g_menu_depth++;
    r = g_menus_orig(type);
    g_menu_depth--;
    return r;
}

static int __attribute__((fastcall)) hk_Controls_SlotActivated(int frontend)
{
    int r;
    g_binding = 1;
    r = g_slot_orig(frontend);
    g_binding = 0;
    return r;
}

/* ---- rumble ----
 * The game's force feedback is for I-Force wheels and only runs with a force-feedback device, but
 * the calls into it happen regardless; these hooks turn them into SDL rumble:
 * - 0x45c910 (fastcall, 2 stack args, ret 8): the local player's car hit something; the second
 *   stack argument is the impact (the caller skips those under 0.02). It clamps that to the
 *   FFB.TXT limits (3.5..4.7) for the wheel's collision force (0x458720), so every bump would
 *   feel the same; the rumble scales the raw impact instead. Measured in bumper cars: ~9 a second
 *   per car, median 0.13 (scraping), 99th percentile 1.0, max 3.7. [Gamepad] ImpactScale = full rumble.
 * - 0x45c6b0 PlayExclusiveFFBEffect(name, flag): power-ups (Afterburner, Mine_Shit, Oil_Slick,
 *   Kangeroo, Ped_Ray, Flamer, Repulse) and 0x45c720 PDPlayFFBEffect(name): Instant_Handbrake.
 * Rumbles fade out over their time; a new one replaces the current one unless that is still stronger. */

typedef void (__attribute__((fastcall)) *collision_fn)(int car, void *normal, void *arg3, float impact);
typedef int (__attribute__((fastcall)) *effect_fn)(const char *name, int flag);
static collision_fn g_collision_orig;
static effect_fn g_exclusive_orig, g_effect_orig;
static float g_rumble, g_impact_scale;
static struct { float low, high; DWORD start, ms, sent; int on; } g_rb;   /* the rumble playing */
static int g_rumble_logged;
static DWORD g_rumble_warn_tick;

/* the one place that calls SDL; checks the result (SDL2: 0 = ok; SDL3: true = ok) and logs the
   first success and (rate-limited) failures, so c2patch.log shows whether rumble reaches the
   hardware. Xbox pads on SDL's RAWINPUT backend fail with "isn't correlated yet" until some input
   has been seen, so an early failure (e.g. the connect pulse) can still become a working rumble. */
static void do_rumble(unsigned short low, unsigned short high, unsigned ms)
{
    int r;
    if (!g_pad || !sdl.rumble) {
        return;
    }
    r = sdl.rumble(g_pad, low, high, ms);
    if (sdl.version == 2 ? r != 0 : (r & 0xff) == 0) {
        DWORD now = GetTickCount();
        if (!g_rumble_warn_tick || now - g_rumble_warn_tick > 5000) {
            g_rumble_warn_tick = now;
            c2_log("gamepad: SDL rumble failed on %s: %s", g_pad_name, sdl.error());
        }
    } else if (low || high) {
        g_rumble_warn_tick = 0;
        if (!g_rumble_logged) {
            g_rumble_logged = 1;
            c2_log("gamepad: rumble reached %s", g_pad_name);
        }
    }
}

/* when a pad is opened, check it claims rumble and give one short pulse: if you feel this, SDL
   rumble works and any missing in-game rumble is the game-event hooks; if not, it's SDL/hardware */
static void rumble_connect_test(void)
{
    unsigned char has = 1;
    if (g_rumble <= 0 || !sdl.rumble) {
        return;
    }
    if (sdl.version == 3 && sdl.sdl3_has_rumble) {
        has = sdl.sdl3_has_rumble(g_pad);
    } else if (sdl.version == 2 && sdl.sdl2_has_rumble) {
        has = sdl.sdl2_has_rumble(g_pad);
    }
    c2_log("gamepad: %s %s rumble; test pulse", g_pad_name, has ? "reports" : "reports NO");
    do_rumble(0x8000, 0x8000, 300);
}

/* a rumble fades out linearly over its time; pad_update re-sends the level every 30 ms */
static float rumble_level(float v, DWORD now)
{
    DWORD t = now - g_rb.start;
    return t >= g_rb.ms ? 0.0f : v * (1.0f - (float)t / (float)g_rb.ms);
}

static void rumble_send(DWORD now)
{
    float low = rumble_level(g_rb.low, now) * g_rumble, high = rumble_level(g_rb.high, now) * g_rumble;
    DWORD left = now - g_rb.start < g_rb.ms ? g_rb.ms - (now - g_rb.start) : 0;
    low = low > 1 ? 1 : low;
    high = high > 1 ? 1 : high;
    g_rb.on = left > 0;
    g_rb.sent = now;
    do_rumble((unsigned short)(low * 65535), (unsigned short)(high * 65535), left ? left + 50 : 0);
}

static void rumble_tick(void)
{
    DWORD now = GetTickCount();
    if (g_rb.on && g_pad && now - g_rb.sent >= 30) {
        rumble_send(now);
    }
}

/* a new rumble takes over unless what is still playing is stronger */
static void rumble(float low, float high, DWORD ms)
{
    DWORD now = GetTickCount();
    float cur_low, cur_high;
    if (!g_pad || !sdl.rumble || g_rumble <= 0) {
        return;
    }
    cur_low = g_rb.on ? rumble_level(g_rb.low, now) : 0;
    cur_high = g_rb.on ? rumble_level(g_rb.high, now) : 0;
    if (low <= cur_low && high <= cur_high) {
        return;
    }
    g_rb.low = low > cur_low ? low : cur_low;
    g_rb.high = high > cur_high ? high : cur_high;
    g_rb.start = now;
    g_rb.ms = ms;
    if (c2_unit) {
        c2_log("gamepad: rumble %.2f/%.2f %lu ms", g_rb.low, g_rb.high, (unsigned long)ms);
    }
    rumble_send(now);
}

static void __attribute__((fastcall)) hk_collision(int car, void *normal, void *arg3, float impact)
{
    float s = impact / g_impact_scale;
    s = s > 1 ? 1 : s;
    if (s >= 0.05f) {           /* scraping along a wall or car gives ~0.1 impacts every frame */
        rumble(s, s * 0.6f, 120 + (DWORD)(s * 230));
    }
    g_collision_orig(car, normal, arg3, impact);
}

static void effect_rumble(const char *name)
{
    static const struct { const char *name; float low, high; DWORD ms; } fx[] = {
        {"Afterburner", 0.5f, 0.8f, 500}, {"Mine_Shit", 0.6f, 0.3f, 200}, {"Oil_Slick", 0.3f, 0.2f, 250},
        {"Kangeroo", 1.0f, 0.5f, 300}, {"Ped_Ray", 0.2f, 0.7f, 400}, {"Flamer", 0.3f, 0.6f, 400},
        {"Repulse", 0.7f, 0.7f, 300}, {"Instant_Handbrake", 0.4f, 0.2f, 150},
    };
    size_t i;
    for (i = 0; i < sizeof fx / sizeof fx[0]; i++) {
        if (name && !strcmp(name, fx[i].name)) {
            rumble(fx[i].low, fx[i].high, fx[i].ms);
            return;
        }
    }
    rumble(0.4f, 0.4f, 200);
}

static int __attribute__((fastcall)) hk_exclusive_effect(const char *name, int flag)
{
    effect_rumble(name);
    return g_exclusive_orig(name, flag);
}

static int __attribute__((fastcall)) hk_effect(const char *name, int unused)
{
    effect_rumble(name);
    return g_effect_orig(name, unused);
}

/* jmp at a function's entry; the returned trampoline runs the moved instructions (which must be
   position independent) and continues after them */
static void *hook_entry(uint32_t addr, const uint8_t *expect, size_t len, void *hook)
{
    uint8_t *t;
    int32_t rel;
    if (!c2_expect(addr, expect, len)) {
        c2_log("gamepad: %08x: original bytes differ, not hooked", addr);
        return NULL;
    }
    t = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) {
        return NULL;
    }
    memcpy(t, expect, len);
    t[len] = 0xe9;
    rel = (int32_t)(addr + len - ((uintptr_t)t + len + 5));
    memcpy(t + len + 1, &rel, 4);
    c2_jmp(addr, hook);
    return t;
}

void c2_gamepad_install(void)
{
    static const uint8_t menus_entry[] = {0xa1, 0x28, 0x23, 0x76, 0x00};                   /* mov eax, [0x762328] */
    static const uint8_t slot_entry[] = {0x83, 0xec, 0x0c, 0xa1, 0x70, 0x87, 0x68, 0x00};  /* sub esp, 12; mov eax, [0x688770] */
    int i;

    g_enabled = c2_ini_int("Gamepad", "Enabled", 1);
    if (!g_enabled) {
        return;
    }
    g_throttle = c2_ini_int("Gamepad", "Throttle", 0);
    g_stick_dz = c2_ini_int("Gamepad", "StickDeadzone", 15) / 100.0f;
    g_trigger_dz = c2_ini_int("Gamepad", "TriggerDeadzone", 5) / 100.0f;
    g_hide_dinput = c2_ini_int("Gamepad", "HideDirectInputJoysticks", 0);
    g_menu_controls = c2_ini_int("Gamepad", "MenuControls", 1);
    g_button_actions = c2_ini_int("Gamepad", "ButtonActions", 1);
    g_stick_dz = g_stick_dz < 0 ? 0 : g_stick_dz > 0.9f ? 0.9f : g_stick_dz;
    g_trigger_dz = g_trigger_dz < 0 ? 0 : g_trigger_dz > 0.9f ? 0.9f : g_trigger_dz;
    for (i = 0; i < PAD_AXES; i++) {
        g_range[i][1] = 0xffff;          /* DirectInput's default joystick range */
    }
    g_dev.lpVtbl = &g_dev_vtbl;
    /* menu-depth / binding tracking also keeps menu_active() right for the in-race button actions,
       so it goes in whenever either feature is on */
    if (g_menu_controls || g_button_actions) {
        g_menus_orig = (fastcall1_fn)hook_entry(0x46d8e0, menus_entry, sizeof menus_entry, (void *)hk_FrontEndProcessMenus);
        g_slot_orig = (fastcall1_fn)hook_entry(0x472b00, slot_entry, sizeof slot_entry, (void *)hk_Controls_SlotActivated);
        if (!g_menus_orig || !g_slot_orig) {
            g_menu_controls = 0;         /* (a half-installed hook still works: it only counts) */
        }
    }
    g_rumble = c2_ini_int("Gamepad", "Rumble", 100) / 100.0f;
    g_impact_scale = c2_ini_float("Gamepad", "ImpactScale", 1.5f);
    g_impact_scale = g_impact_scale < 0.1f ? 0.1f : g_impact_scale;
    if (g_rumble > 0) {
        static const uint8_t ffb_entry[] = {0xa1, 0x68, 0x61, 0x68, 0x00};                        /* mov eax, [gForceFeedbackAvailable] */
        static const uint8_t impact_entry[] = {0xd9, 0x44, 0x24, 0x08, 0xd9, 0x44, 0x24, 0x08};  /* fld [esp+8]; fld [esp+8] */
        g_collision_orig = (collision_fn)hook_entry(0x45c910, impact_entry, sizeof impact_entry, (void *)hk_collision);
        g_exclusive_orig = (effect_fn)hook_entry(0x45c6b0, ffb_entry, sizeof ffb_entry, (void *)hk_exclusive_effect);
        g_effect_orig = (effect_fn)hook_entry(0x45c720, ffb_entry, sizeof ffb_entry, (void *)hk_effect);
    }
    c2_log("gamepad: SDL gamepads on (throttle %s, dead zones %d%%/%d%%%s%s, rumble %d%%)",
           g_throttle == 0 ? "triggers" : g_throttle == 1 ? "left stick" : "triggers or left stick",
           (int)(g_stick_dz * 100 + 0.5f), (int)(g_trigger_dz * 100 + 0.5f), g_menu_controls ? ", menu controls" : "",
           g_button_actions ? ", game buttons" : "", (int)(g_rumble * 100 + 0.5f));
}

int c2_gamepad_enabled(void)
{
    return g_enabled;
}
