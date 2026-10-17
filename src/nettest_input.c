/* nettest input: scripted keys for background test instances.
 *
 * The DirectInput keyboard is switched to background/non-exclusive, and GetDeviceState for
 * the keyboard (256-byte state) gets the scripted keys added. Commands are read from
 * c2cmd_unit<N>.txt in the game folder (written by tools/mp.py; the file is deleted once read):
 *     tap <DIK>[,<DIK>...] [hold ms]  press (up to 4 keys together) and release; queued, 100 ms gap
 *     text <ascii>                  types letters/digits/space (shift not supported)
 *     slow <ms>                     sleep this long every frame (emulates a slow PC; 0 = off)
 */
#include <stdio.h>
#include <string.h>
#include "c2patch.h"

#define QUEUE 512

static struct { unsigned char dik[4]; int hold; } g_queue[QUEUE];
static volatile int g_q_head, g_q_tail;
static volatile unsigned char g_down[4];       /* currently held DIKs, 0 = none */
static CRITICAL_SECTION g_lock;
static char g_cmd_path[MAX_PATH];

typedef HRESULT (WINAPI *GetDeviceState_fn)(void *, DWORD, void *);
typedef HRESULT (WINAPI *SetCooperativeLevel_fn)(void *, HWND, DWORD);
typedef HRESULT (WINAPI *CreateDevice_fn)(void *, const GUID *, void **, void *);
static GetDeviceState_fn real_GetDeviceState;
static SetCooperativeLevel_fn real_SetCooperativeLevel;
static CreateDevice_fn real_CreateDevice;

static volatile long g_calls, g_kbd_calls, g_last_hr;
static volatile int g_slow_ms;   /* "slow <ms>": extra time per frame, to emulate slow PCs */

static HRESULT WINAPI hk_GetDeviceState(void *dev, DWORD size, void *data)
{
    HRESULT hr = real_GetDeviceState(dev, size, data);
    g_calls++;
    if (size == 256) {
        g_kbd_calls++;
        if (g_slow_ms > 0) {
            Sleep(g_slow_ms);     /* the keyboard is polled once per frame */
        }
        g_last_hr = hr;
    }
    if (size == 256) {
        if (hr != 0) {
            memset(data, 0, 256);
            hr = 0;
        }
        int i;
        for (i = 0; i < 4; i++) {
            if (g_down[i]) {
                ((unsigned char *)data)[g_down[i]] = 0x80;
            }
        }
    }
    return hr;
}

static HRESULT WINAPI hk_SetCooperativeLevel(void *dev, HWND hwnd, DWORD flags)
{
    (void)flags;
    return real_SetCooperativeLevel(dev, hwnd, 0x8 | 0x2);   /* DISCL_BACKGROUND | DISCL_NONEXCLUSIVE */
}

static void patch_vtable(void **vtbl, int slot, void *fn, void **old)
{
    if (vtbl[slot] == fn) {
        return;
    }
    *old = vtbl[slot];
    c2_write((uint32_t)(uintptr_t)&vtbl[slot], &fn, 4);
}

static HRESULT WINAPI hk_CreateDevice(void *di, const GUID *guid, void **out, void *outer)
{
    HRESULT hr = real_CreateDevice(di, guid, out, outer);
    if (hr == 0 && *out) {
        void **vtbl = *(void ***)*out;
        patch_vtable(vtbl, 9, hk_GetDeviceState, (void **)&real_GetDeviceState);
        patch_vtable(vtbl, 13, hk_SetCooperativeLevel, (void **)&real_SetCooperativeLevel);
    }
    return hr;
}

/* EnumDevices: hide game controllers, so the test units drive from the (scripted) keyboard
   instead of a real pad's centred axes */
typedef BOOL (CALLBACK *EnumCb)(const void *inst, void *ref);
typedef HRESULT (WINAPI *EnumDevices_fn)(void *, DWORD, EnumCb, void *, DWORD);
static EnumDevices_fn real_EnumDevices;
static EnumCb g_enum_cb;

static BOOL CALLBACK filter_cb(const void *inst, void *ref)
{
    DWORD type = *(const DWORD *)((const char *)inst + 4 + 16 + 16);  /* DIDEVICEINSTANCEA.dwDevType */
    if ((type & 0xff) == 4 || (type & 0xff) == 5) {                  /* DIDEVTYPE_JOYSTICK / _DEVICE */
        return TRUE;
    }
    return g_enum_cb(inst, ref);
}

static HRESULT WINAPI hk_EnumDevices(void *di, DWORD type, EnumCb cb, void *ref, DWORD flags)
{
    g_enum_cb = cb;
    return real_EnumDevices(di, type, filter_cb, ref, flags);
}

/* called by the dinput proxy with the IDirectInputA the game gets */
void c2_nettest_wrap_dinput(void *di)
{
    if (!c2_unit || !di) {
        return;
    }
    patch_vtable(*(void ***)di, 3, hk_CreateDevice, (void **)&real_CreateDevice);
    patch_vtable(*(void ***)di, 4, hk_EnumDevices, (void **)&real_EnumDevices);
}

static void push4(const unsigned char dik[4], int hold)
{
    EnterCriticalSection(&g_lock);
    if ((g_q_tail + 1) % QUEUE != g_q_head) {
        memcpy(g_queue[g_q_tail].dik, dik, 4);
        g_queue[g_q_tail].hold = hold;
        g_q_tail = (g_q_tail + 1) % QUEUE;
    }
    LeaveCriticalSection(&g_lock);
}

static void push(unsigned char dik, int hold)
{
    unsigned char d[4] = {0, 0, 0, 0};
    d[0] = dik;
    push4(d, hold);
}

static unsigned char ascii_dik(char c)
{
    static const char row1[] = "1234567890", qrow[] = "qwertyuiop", arow[] = "asdfghjkl", zrow[] = "zxcvbnm";
    const char *p;
    if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
    }
    if ((p = strchr(row1, c)) != NULL && c) return (unsigned char)(0x02 + (p - row1));
    if ((p = strchr(qrow, c)) != NULL && c) return (unsigned char)(0x10 + (p - qrow));
    if ((p = strchr(arow, c)) != NULL && c) return (unsigned char)(0x1e + (p - arow));
    if ((p = strchr(zrow, c)) != NULL && c) return (unsigned char)(0x2c + (p - zrow));
    if (c == ' ') return 0x39;
    return 0;
}

static void read_commands(void)
{
    char line[256];
    FILE *f = fopen(g_cmd_path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof line, f)) {
        unsigned k[4] = {0, 0, 0, 0};
        int hold = 120, n;
        if ((n = sscanf(line, "tap %x,%x,%x,%x %d", &k[0], &k[1], &k[2], &k[3], &hold)) >= 1 && n == 5) {
            unsigned char d[4] = {(unsigned char)k[0], (unsigned char)k[1], (unsigned char)k[2], (unsigned char)k[3]};
            push4(d, hold);
        } else if ((n = sscanf(line, "tap %x,%x,%x %d", &k[0], &k[1], &k[2], &hold)) == 4) {
            unsigned char d[4] = {(unsigned char)k[0], (unsigned char)k[1], (unsigned char)k[2], 0};
            push4(d, hold);
        } else if ((n = sscanf(line, "tap %x,%x %d", &k[0], &k[1], &hold)) == 3) {
            unsigned char d[4] = {(unsigned char)k[0], (unsigned char)k[1], 0, 0};
            push4(d, hold);
        } else if (sscanf(line, "tap %x %d", &k[0], &hold) >= 1) {
            push((unsigned char)k[0], hold);
        } else if (sscanf(line, "slow %d", &hold) == 1) {
            g_slow_ms = hold;
            c2_log("input: slow %d ms per frame", hold);
        } else if (strncmp(line, "text ", 5) == 0) {
            char *c;
            for (c = line + 5; *c && *c != '\n' && *c != '\r'; c++) {
                unsigned char d = ascii_dik(*c);
                if (d) {
                    push(d, 80);
                }
            }
        }
    }
    fclose(f);
    DeleteFileA(g_cmd_path);
}

static DWORD WINAPI input_thread(LPVOID arg)
{
    DWORD last_log = 0;
    (void)arg;
    for (;;) {
        if (GetTickCount() - last_log > 5000) {
            last_log = GetTickCount();
            c2_log("input: GetDeviceState calls %ld, keyboard %ld (last hr %08lx); crush list max slot %d, slot 7 filled %d times",
                   g_calls, g_kbd_calls, g_last_hr, c2_probe_crush_max, c2_probe_crush_slot7);
        }
        read_commands();
        if (g_q_head != g_q_tail) {
            unsigned char dik[4];
            int hold;
            EnterCriticalSection(&g_lock);
            memcpy(dik, g_queue[g_q_head].dik, 4);
            hold = g_queue[g_q_head].hold;
            g_q_head = (g_q_head + 1) % QUEUE;
            LeaveCriticalSection(&g_lock);
            memcpy((void *)g_down, dik, 4);
            Sleep(hold);
            memset((void *)g_down, 0, 4);
            Sleep(100);
        } else {
            Sleep(50);
        }
    }
    return 0;
}

void c2_nettest_input_start(const char *game_dir)
{
    InitializeCriticalSection(&g_lock);
    sprintf(g_cmd_path, "%sc2cmd_unit%d.txt", game_dir, c2_unit);
    DeleteFileA(g_cmd_path);
    CloseHandle(CreateThread(NULL, 0, input_thread, NULL, 0, NULL));
}
