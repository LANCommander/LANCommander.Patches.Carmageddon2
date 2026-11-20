#ifndef C2PATCH_H
#define C2PATCH_H
#include <windows.h>
#include <stdint.h>

void c2_patch_init(HINSTANCE dll, HMODULE interposer);   /* interposer: NULL as the dinput.dll proxy */
void c2_log(const char *fmt, ...);

/* memory helpers (all addresses are CARMA2_HW.EXE VAs, image base 0x400000, no relocations) */
int c2_write(uint32_t addr, const void *bytes, size_t len);
int c2_expect(uint32_t addr, const void *bytes, size_t len);   /* 1 if memory matches */
int c2_patch(uint32_t addr, const void *expect, const void *bytes, size_t len);
int c2_write32(uint32_t addr, uint32_t value);
int c2_jmp(uint32_t from, const void *to);    /* E9 rel32 at from */
int c2_call(uint32_t from, const void *to);   /* E8 rel32 at from */

/* settings: c2patch.ini next to this DLL; as an Interposer plugin, then Config.yml Plugins.c2patch.<section>.<key> */
int c2_ini_int(const char *section, const char *key, int def);
float c2_ini_float(const char *section, const char *key, float def);
void c2_ini_write(const char *section, const char *key, const char *value);

void *c2_hook_import(const char *dll, const char *name, void *replacement);  /* returns the old target */

/* patch modules */
extern int c2_unit;   /* nettest unit (C2_UNIT), 0 when not testing */
void c2_nettest_install(void);
void c2_probe_install(void);
extern volatile int c2_probe_crush_max, c2_probe_crush_slot7;
extern int c2_crush_limit;
void c2_nettest_wrap_dinput(void *direct_input);
void c2_nettest_input_start(const char *game_dir);
void c2_render_install(void);
void c2_ai_install(void);
void c2_net_install(void);
void c2_netrate_install(void);
void c2_names_install(void);
void c2_gamepad_install(void);
void c2_gamepad_wrap_dinput(void *direct_input);
int c2_gamepad_enabled(void);
int c2_gamepad_is_virtual(const void *guid);
int c2_gamepad_test(int is_axis, int index, int value);
extern volatile long c2_netrate_sends;

#define C2_ADDR(a) ((void *)(uintptr_t)(a))
#endif
