/* Core of the patch: verifies the exe, reads c2patch.ini, installs the patch modules. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "c2patch.h"

#define C2_TIMESTAMP 0x3662d4f7u   /* CARMA2_HW.EXE (GOG, SafeDisc removed) */

static char g_ini[MAX_PATH];
static char g_log[MAX_PATH];

void c2_log(const char *fmt, ...)
{
    FILE *f = fopen(g_log, "a");
    va_list ap;
    if (!f) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

int c2_write(uint32_t addr, const void *bytes, size_t len)
{
    DWORD old;
    if (!VirtualProtect(C2_ADDR(addr), len, PAGE_EXECUTE_READWRITE, &old)) {
        c2_log("VirtualProtect %08x failed (%lu)", addr, GetLastError());
        return 0;
    }
    memcpy(C2_ADDR(addr), bytes, len);
    VirtualProtect(C2_ADDR(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), C2_ADDR(addr), len);
    return 1;
}

int c2_expect(uint32_t addr, const void *bytes, size_t len)
{
    return memcmp(C2_ADDR(addr), bytes, len) == 0;
}

int c2_patch(uint32_t addr, const void *expect, const void *bytes, size_t len)
{
    if (!c2_expect(addr, expect, len)) {
        c2_log("patch %08x: original bytes differ, skipped", addr);
        return 0;
    }
    return c2_write(addr, bytes, len);
}

int c2_write32(uint32_t addr, uint32_t value)
{
    return c2_write(addr, &value, 4);
}

static int rel32(uint32_t from, uint8_t op, const void *to)
{
    uint8_t b[5];
    int32_t rel = (int32_t)((uintptr_t)to - (from + 5));
    b[0] = op;
    memcpy(b + 1, &rel, 4);
    return c2_write(from, b, 5);
}

int c2_jmp(uint32_t from, const void *to) { return rel32(from, 0xe9, to); }
int c2_call(uint32_t from, const void *to) { return rel32(from, 0xe8, to); }

int c2_ini_int(const char *section, const char *key, int def)
{
    return (int)GetPrivateProfileIntA(section, key, def, g_ini);
}

float c2_ini_float(const char *section, const char *key, float def)
{
    char buf[64];
    GetPrivateProfileStringA(section, key, "", buf, sizeof buf, g_ini);
    return buf[0] ? (float)atof(buf) : def;
}

static int exe_is_supported(void)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)dos + dos->e_lfanew);
    return (uintptr_t)dos == 0x400000 && nt->FileHeader.TimeDateStamp == C2_TIMESTAMP
        && nt->OptionalHeader.AddressOfEntryPoint == 0x1788a0;
}

void c2_patch_init(HINSTANCE dll)
{
    char *slash;
    GetModuleFileNameA(dll, g_ini, MAX_PATH);
    slash = strrchr(g_ini, '\\');
    if (slash) {
        slash[1] = '\0';
    }
    strcpy(g_log, g_ini);
    strcat(g_ini, "c2patch.ini");
    {
        char unit[8];
        if (GetEnvironmentVariableA("C2_UNIT", unit, sizeof unit)) {
            strcat(g_log, "c2patch_unit");
            strcat(g_log, unit);
            strcat(g_log, ".log");
        } else {
            strcat(g_log, "c2patch.log");
        }
    }
    DeleteFileA(g_log);

    if (!exe_is_supported()) {
        c2_log("c2patch: not the supported CARMA2_HW.EXE; no patches applied");
        return;
    }
    if (!c2_ini_int("General", "Enabled", 1)) {
        c2_log("c2patch: disabled in c2patch.ini");
        return;
    }
    c2_log("c2patch: CARMA2_HW.EXE recognised, installing");
    c2_nettest_install();
    c2_render_install();
    c2_ai_install();
    c2_net_install();
}
