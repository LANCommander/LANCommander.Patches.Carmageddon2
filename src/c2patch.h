#ifndef C2PATCH_H
#define C2PATCH_H
#include <windows.h>
#include <stdint.h>

void c2_patch_init(HINSTANCE dll);
void c2_log(const char *fmt, ...);

/* memory helpers (all addresses are CARMA2_HW.EXE VAs, image base 0x400000, no relocations) */
int c2_write(uint32_t addr, const void *bytes, size_t len);
int c2_expect(uint32_t addr, const void *bytes, size_t len);   /* 1 if memory matches */
int c2_patch(uint32_t addr, const void *expect, const void *bytes, size_t len);
int c2_write32(uint32_t addr, uint32_t value);
int c2_jmp(uint32_t from, const void *to);    /* E9 rel32 at from */
int c2_call(uint32_t from, const void *to);   /* E8 rel32 at from */

/* ini settings (c2patch.ini next to the exe) */
int c2_ini_int(const char *section, const char *key, int def);
float c2_ini_float(const char *section, const char *key, float def);

/* patch modules */
void c2_render_install(void);
void c2_ai_install(void);
void c2_net_install(void);

#define C2_ADDR(a) ((void *)(uintptr_t)(a))
#endif
