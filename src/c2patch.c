/* Core of the patch: verifies the exe, reads the settings, installs the patch modules. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c2patch.h"

#define C2_TIMESTAMP 0x3662d4f7u   /* CARMA2_HW.EXE (GOG, SafeDisc removed) */

#ifndef C2PATCH_VERSION
#define C2PATCH_VERSION "dev"      /* overridden by build.sh from CARMA2_VERSION */
#endif

static char g_ini[MAX_PATH];
static char g_log[MAX_PATH];

/* LANCommander Interposer API (its exports are __cdecl) */
typedef void (*InterposerLog_fn)(const wchar_t *verb, const wchar_t *message);
typedef BOOL (*InterposerRegisterPluginConfig_fn)(const wchar_t *plugin, const wchar_t *yaml);
static InterposerLog_fn g_ilog;

/* Plugins.Carmageddon2 defaults for Config.yml, generated from dist/c2patch.ini by build.sh */
static const wchar_t g_yaml_defaults[] =
#include "c2patch_yaml.h"
    ;

/* Plugins.Carmageddon2.<section>.<key> from Config.yml. Read directly rather than through
 * InterposerGetConfigString: as of Interposer 1.2.1 only its first call succeeds (its path walk
 * does `node = node[seg]`, and yaml-cpp's Node assignment rewrites the root it was copied from).
 * Handles the block-style maps Interposer writes, with comments and quoted scalars. */
static struct yml_setting { char section[32], key[48], value[64]; } g_yml[64];
static int g_yml_count;

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) {
        *--e = '\0';
    }
    return s;
}

struct yml_map { int indent; char name[32]; };   /* the maps enclosing the current line */

static void yml_line(char *line, struct yml_map *stack, int *depth)
{
    char *colon, *key, *value, *hash;
    int ind = 0;

    while (line[ind] == ' ') {
        ind++;
    }
    if (line[ind] == '#' || line[ind] == '-' || !(colon = strchr(line + ind, ':'))) {
        return;
    }
    *colon = '\0';
    key = trim(line + ind);
    value = trim(colon + 1);
    if (*value == '\'' || *value == '"') {
        char *end = strchr(value + 1, *value);
        value++;
        if (end) {
            *end = '\0';
        }
    } else if ((hash = strstr(value, " #")) != NULL || *value == '#') {
        *(*value == '#' ? value : hash) = '\0';
        value = trim(value);
    }
    while (*depth > 0 && ind <= stack[*depth - 1].indent) {
        (*depth)--;
    }
    if (!*value) {
        if (*depth < 8) {
            stack[*depth].indent = ind;
            lstrcpynA(stack[*depth].name, key, sizeof stack[0].name);
            (*depth)++;
        }
    } else if (*depth == 3 && !strcmp(stack[0].name, "Plugins") && !strcmp(stack[1].name, "Carmageddon2")
               && g_yml_count < (int)(sizeof g_yml / sizeof g_yml[0])) {
        struct yml_setting *s = &g_yml[g_yml_count++];
        lstrcpynA(s->section, stack[2].name, sizeof s->section);
        lstrcpynA(s->key, key, sizeof s->key);
        lstrcpynA(s->value, value, sizeof s->value);
    }
}

static void yml_load(HMODULE interposer)
{
    wchar_t path[MAX_PATH + 32], *slash;
    HANDLE f;
    DWORD size, got = 0;
    char *text, *line, *next;
    struct yml_map stack[8];
    int depth = 0;

    GetModuleFileNameW(interposer, path, MAX_PATH);
    slash = wcsrchr(path, L'\\');
    lstrcpyW(slash ? slash + 1 : path, L".interposer\\Config.yml");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    size = GetFileSize(f, NULL);
    text = size < (4u << 20) ? malloc(size + 1) : NULL;
    if (text && ReadFile(f, text, size, &got, NULL)) {
        text[got] = '\0';
        line = strncmp(text, "\xef\xbb\xbf", 3) == 0 ? text + 3 : text;
        for (; line; line = next) {
            next = strchr(line, '\n');
            if (next) {
                *next++ = '\0';
            }
            if (*trim(line)) {
                yml_line(line, stack, &depth);
            }
        }
    }
    free(text);
    CloseHandle(f);
}

void c2_log(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    FILE *f;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    f = fopen(g_log, "a");
    if (f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
    if (g_ilog) {
        wchar_t w[1024];
        if (MultiByteToWideChar(CP_ACP, 0, line, -1, w, 1024)) {
            g_ilog(L"CARMA2", w);
        }
    }
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

/* c2patch.ini first, so the values the game's menus save there win; then Config.yml */
static int setting(const char *section, const char *key, char *buf, DWORD size)
{
    int i;
    GetPrivateProfileStringA(section, key, "", buf, size, g_ini);
    for (i = 0; !buf[0] && i < g_yml_count; i++) {
        if (!lstrcmpiA(g_yml[i].section, section) && !lstrcmpiA(g_yml[i].key, key)) {
            lstrcpynA(buf, g_yml[i].value, size);
        }
    }
    return buf[0] != '\0';
}

int c2_ini_int(const char *section, const char *key, int def)
{
    char buf[64];
    if (!setting(section, key, buf, sizeof buf)) {
        return def;
    }
    if (!lstrcmpiA(buf, "true") || !lstrcmpiA(buf, "yes") || !lstrcmpiA(buf, "on")) {
        return 1;
    }
    return atoi(buf);   /* YAML false/no/off parse as 0 */
}

float c2_ini_float(const char *section, const char *key, float def)
{
    char buf[64];
    return setting(section, key, buf, sizeof buf) ? (float)atof(buf) : def;
}

void c2_ini_write(const char *section, const char *key, const char *value)
{
    WritePrivateProfileStringA(section, key, value, g_ini);
}

static int exe_is_supported(void)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)((const char *)dos + dos->e_lfanew);
    return (uintptr_t)dos == 0x400000 && nt->FileHeader.TimeDateStamp == C2_TIMESTAMP
        && nt->OptionalHeader.AddressOfEntryPoint == 0x1788a0;
}

void c2_patch_init(HINSTANCE dll, HMODULE interposer)
{
    char *slash;
    char mutex[48];

    /* both the dinput.dll proxy and the plugin can be present (Interposer injected by its
     * Injector.exe rather than deployed as dinput.dll); the first one patches */
    wsprintfA(mutex, "Local\\c2patch.%lu", GetCurrentProcessId());
    if (CreateMutexA(NULL, FALSE, mutex) && GetLastError() == ERROR_ALREADY_EXISTS) {
        return;
    }
    if (interposer) {
        InterposerRegisterPluginConfig_fn reg = (InterposerRegisterPluginConfig_fn)(void (*)(void))
            GetProcAddress(interposer, "InterposerRegisterPluginConfig");
        g_ilog = (InterposerLog_fn)(void (*)(void))GetProcAddress(interposer, "InterposerLog");
        if (reg) {
            reg(L"Carmageddon2", g_yaml_defaults);   /* writes them to Config.yml on the first run */
        }
        yml_load(interposer);
    }
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
    c2_log("c2patch %s", C2PATCH_VERSION);

    if (!exe_is_supported()) {
        c2_log("c2patch: not the supported CARMA2_HW.EXE; no patches applied");
        return;
    }
    if (!c2_ini_int("General", "Enabled", 1)) {
        c2_log("c2patch: disabled in settings");
        return;
    }
    c2_log("c2patch: CARMA2_HW.EXE recognised, installing%s", interposer ? " (Interposer plugin)" : "");
    c2_nettest_install();
    c2_render_install();
    c2_ai_install();
    c2_net_install();
    c2_netrate_install();
    c2_names_install();
    c2_gamepad_install();
}
