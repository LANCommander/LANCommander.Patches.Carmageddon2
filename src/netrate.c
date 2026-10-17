/* Network update rate.
 *
 * Car state goes out from ApplyPhysicsToCars (0x416340) once per rendered frame, after the
 * physics steps (25 Hz, 40 ms: PHILDoPhysics adds 0x28 to gPHIL_last_physics_tick), when
 * TimeToSendData (0x4978d0) says 80 ms (0x50) of physics time have passed since the last send:
 * 12.5 Hz, or once per frame below 12.5 fps. The host broadcasts every player's car
 * (SendCarData 0x497900); a client sends only its own car to the host.
 *
 * [Net] SendIntervalMs replaces the 80 ms (physics ticks are 40 ms apart, so 40 = 25 Hz is the
 * fastest that sends new data; 80 = stock; 120 = 8.3 Hz). It can also be set in the game:
 * NET GAME OPTIONS gets an UPDATE RATE row (25 HZ / 12 HZ / 8 HZ), saved back to c2patch.ini.
 * The setting is per machine: the host's value decides how often everyone sees the others.
 *
 * Stock TimeToSendData also sends whenever the physics tick hasn't moved since the last send
 * (`last < tick` fails when equal; meant for resets). At 60 fps the frames after a send usually
 * have no new physics step, so the same car state goes out again with the same timestamp:
 * measured ~30 packets/s for 12.5 distinct updates/s. [Net] SkipDuplicateSends (default 1)
 * drops those; a tick that goes backwards (reset) still sends.
 */
#include <stdio.h>
#include <string.h>
#include "c2patch.h"

#define gPHIL_last_physics_tick   (*(volatile uint32_t *)0x74a5ec)
#define last_physics_send_time    (*(uint32_t *)0x659c30)

#define SPEC_NETWORK_OPTIONS      0x610208     /* gFrontend_NETWORK_OPTIONS */
#define SPEC_COUNT_ITEMS(s)       (*(int *)((s) + 0x104))
#define SPEC_CREATE(s)            (*(uint32_t *)((s) + 0x108))
#define SPEC_ITEM(s, i)           ((s) + 0x130 + (i) * 0x158)
#define SPEC_COUNT_RADIOS(s)      (*(int *)((s) + 0x9be4))
#define SPEC_RADIO(s, i)          ((s) + 0x9be8 + (i) * 0x2c)

#define ITEM_STRING_ID(it)        (*(int *)((it) + 0x00))
#define ITEM_ACTION(it)           (*(uint32_t *)((it) + 0x04))
#define ITEM_X(it)                (*(int *)((it) + 0x18))
#define ITEM_Y(it)                (*(int *)((it) + 0x1c))
#define ITEM_Y2(it)               (*(int *)((it) + 0x54))   /* copy of y (morph target) */
#define ITEM_GROUP(it)            (*(int *)((it) + 0x40))
#define ITEM_TEXT(it)             ((char *)((it) + 0x59))
#define ITEM_SIZE                 0x158
#define STRING_USE_TEXT           1028

#define RATE_GROUP                8            /* radio group of the new row (radios[7]) */
#define FIRST_NEW                 31           /* label, 3 choices; OK/CANCEL move to 35/36 */

typedef int (__fastcall *menu_fn)(uintptr_t spec);
static const menu_fn NetOptions_Infunc = (menu_fn)0x472ed0;
static const menu_fn NetOptions_Ok = (menu_fn)0x473470;
typedef void (__fastcall *select_fn)(uintptr_t spec, int group, int item);
typedef int (__fastcall *which_fn)(uintptr_t spec, int group);
static const select_fn SelectThisItemIn = (select_fn)0x484f70;
static const which_fn WhichItemIsSelectedIn = (which_fn)0x484f40;
typedef void (__fastcall *widths_fn)(uintptr_t spec);
static const widths_fn FuckWithWidths = (widths_fn)0x4666f0;

static const int g_choices_ms[3] = {40, 80, 120};
static const char *const g_choice_text[3] = {"25 hz", "12 hz", "8 hz"};
static volatile uint32_t g_interval_ms = 80;
static int g_skip_duplicates = 1;
volatile long c2_netrate_sends;   /* car-data sends so far (nettest logs it) */

static int __fastcall hk_TimeToSendData(void)
{
    uint32_t tick = gPHIL_last_physics_tick;
    if (last_physics_send_time < tick && tick < last_physics_send_time + g_interval_ms) {
        return 0;
    }
    if (g_skip_duplicates && tick == last_physics_send_time) {
        return 0;   /* no physics step since the last send: same data, same timestamp */
    }
    last_physics_send_time = tick;
    c2_netrate_sends++;
    return 1;
}

static void set_interval(int ms)
{
    if (ms < 1) {
        ms = 1;
    }
    g_interval_ms = (uint32_t)ms;
}

static int choice_for_interval(void)
{
    int i, best = 1;
    for (i = 0; i < 3; i++) {
        if ((int)g_interval_ms >= g_choices_ms[i] - 20) {
            best = i;
        }
    }
    return best;
}

static int __fastcall hk_NetOptions_Ok(uintptr_t spec)
{
    int ret = NetOptions_Ok(spec);
    int sel = WhichItemIsSelectedIn(spec, RATE_GROUP) - (FIRST_NEW + 1);
    if (sel >= 0 && sel < 3 && (int)g_interval_ms != g_choices_ms[sel]) {
        char buf[16];
        set_interval(g_choices_ms[sel]);
        sprintf(buf, "%d", g_choices_ms[sel]);
        c2_ini_write("Net", "SendIntervalMs", buf);
        c2_log("netrate: update interval %d ms (menu)", g_choices_ms[sel]);
    }
    return ret;
}

/* Adds the UPDATE RATE row once the menu's items are loaded, before FRONTEND_CreateMenu builds
   their text actors (it reads count_items after calling this). */
static int __fastcall hk_NetOptions_Infunc(uintptr_t spec)
{
    int ret = NetOptions_Infunc(spec);
    int i;

    if (SPEC_COUNT_ITEMS(spec) == 33 && SPEC_COUNT_RADIOS(spec) == 7) {
        uint8_t ok[ITEM_SIZE], cancel[ITEM_SIZE];

        /* tighten the 9 option rows (y = 100 + 28k, the -/+ arrows 6 lower) to 96 + 24k */
        for (i = 1; i <= 30; i++) {
            uintptr_t it = SPEC_ITEM(spec, i);
            int arrow = ITEM_STRING_ID(it) == 1027;
            int k = (ITEM_Y(it) - (arrow ? 6 : 0) - 100 + 14) / 28;
            ITEM_Y(it) = ITEM_Y2(it) = 96 + 24 * k + (arrow ? 6 : 0);
        }
        memcpy(ok, (void *)SPEC_ITEM(spec, 31), ITEM_SIZE);
        memcpy(cancel, (void *)SPEC_ITEM(spec, 32), ITEM_SIZE);

        /* label (from "Car types") and three choices (from "eagle / other / all") */
        memcpy((void *)SPEC_ITEM(spec, FIRST_NEW), (void *)SPEC_ITEM(spec, 19), ITEM_SIZE);
        for (i = 0; i < 3; i++) {
            memcpy((void *)SPEC_ITEM(spec, FIRST_NEW + 1 + i), (void *)SPEC_ITEM(spec, 20 + i), ITEM_SIZE);
        }
        for (i = 0; i < 4; i++) {
            uintptr_t it = SPEC_ITEM(spec, FIRST_NEW + i);
            ITEM_Y(it) = ITEM_Y2(it) = 96 + 24 * 9;
            ITEM_STRING_ID(it) = STRING_USE_TEXT;
            if (i > 0) {
                ITEM_GROUP(it) = RATE_GROUP;
            }
            strcpy(ITEM_TEXT(it), i == 0 ? "Update rate:" : g_choice_text[i - 1]);
        }
        memcpy((void *)SPEC_ITEM(spec, 35), ok, ITEM_SIZE);
        memcpy((void *)SPEC_ITEM(spec, 36), cancel, ITEM_SIZE);
        ITEM_ACTION(SPEC_ITEM(spec, 35)) = (uint32_t)(uintptr_t)hk_NetOptions_Ok;

        memcpy((void *)SPEC_RADIO(spec, 7), (void *)SPEC_RADIO(spec, 6), 0x2c);
        *(int *)(SPEC_RADIO(spec, 7) + 8) = FIRST_NEW + 1;      /* indexFirstItem */
        *(int *)(SPEC_RADIO(spec, 7) + 12) = FIRST_NEW + 3;     /* indexLastItem */
        SPEC_COUNT_RADIOS(spec) = 8;
        SPEC_COUNT_ITEMS(spec) = 37;
    }
    if (SPEC_COUNT_ITEMS(spec) == 37) {
        SelectThisItemIn(spec, RATE_GROUP, FIRST_NEW + 1 + choice_for_interval());
        FuckWithWidths(spec);
    }
    return ret;
}

void c2_netrate_install(void)
{
    static const uint8_t time_to_send[] = {0xa1, 0xec, 0xa5, 0x74, 0x00, 0x8b, 0x0d, 0x30, 0x9c, 0x65, 0x00};

    set_interval(c2_ini_int("Net", "SendIntervalMs", 80));
    g_skip_duplicates = c2_ini_int("Net", "SkipDuplicateSends", 1);
    if (!c2_expect(0x4978d0, time_to_send, sizeof time_to_send)) {
        c2_log("netrate: TimeToSendData differs; not installed");
        return;
    }
    c2_jmp(0x4978d0, hk_TimeToSendData);
    if (c2_ini_int("Net", "UpdateRateMenu", 1) && SPEC_CREATE(SPEC_NETWORK_OPTIONS) == 0x472ed0) {
        uint32_t fn = (uint32_t)(uintptr_t)hk_NetOptions_Infunc;
        c2_write(SPEC_NETWORK_OPTIONS + 0x108, &fn, 4);
    }
    c2_log("netrate: update interval %u ms%s", (unsigned)g_interval_ms,
           g_skip_duplicates ? ", duplicate sends skipped" : "");
}
