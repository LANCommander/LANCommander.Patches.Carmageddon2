/* Local multiplayer test harness: several game instances on one PC.
 *
 * Only active when the environment variable C2_UNIT=1..16 is set:
 * - IPX loopback: the game's IPX socket becomes a UDP socket on 127.0.0.1:(C2_PORT_BASE + unit).
 *   IPX node 02:00:00:00:00:<unit> on network 00000001; broadcasts go to every other unit's port.
 *   (GOG's ipxwrapper gives every instance on a PC the same node/socket, so it can't do this.)
 * - Multiple instances: FindWindowA(game class) reports no other window.
 * - Background instances keep running: the deactivation messages that make the game block in
 *   GetMessage (WM_ACTIVATEAPP off, WM_ACTIVATE inactive, minimise) never reach its window proc.
 * - Windows are tiled by unit (C2_TILE=0 turns that off).
 */
#include <winsock.h>
#include <stdio.h>
#include <string.h>
#include "c2patch.h"

#define AF_IPX_ 6
#define MAX_UNITS 16

typedef struct {
    short sa_family;
    char sa_netnum[4];
    unsigned char sa_nodenum[6];
    unsigned short sa_socket;
} ipx_addr;

int c2_unit;
static int g_port_base = 47100;
static SOCKET g_ipx = INVALID_SOCKET;
static unsigned short g_ipx_socket;
static WNDPROC g_game_wndproc;

static int (WINAPI *real_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static int (WINAPI *real_recvfrom)(SOCKET, char *, int, int, struct sockaddr *, int *);
static SOCKET (WINAPI *real_socket)(int, int, int);
static int (WINAPI *real_bind)(SOCKET, const struct sockaddr *, int);
static int (WINAPI *real_getsockname)(SOCKET, struct sockaddr *, int *);
static int (WINAPI *real_closesocket)(SOCKET);
static ATOM (WINAPI *real_RegisterClassA)(const WNDCLASSA *);
static BOOL (WINAPI *real_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);

static void fill_ipx(ipx_addr *a, int unit)
{
    memset(a, 0, sizeof *a);
    a->sa_family = AF_IPX_;
    a->sa_netnum[3] = 1;
    a->sa_nodenum[0] = 2;
    a->sa_nodenum[5] = (unsigned char)unit;
    a->sa_socket = g_ipx_socket;
}

static struct sockaddr_in udp_addr(int unit)
{
    struct sockaddr_in in;
    memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    in.sin_port = htons((unsigned short)(g_port_base + unit));
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return in;
}

static SOCKET WINAPI hk_socket(int af, int type, int proto)
{
    struct sockaddr_in in;
    SOCKET s;
    if (af != AF_IPX_) {
        return real_socket(af, type, proto);
    }
    s = real_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    in = udp_addr(c2_unit);
    if (s != INVALID_SOCKET && real_bind(s, (struct sockaddr *)&in, sizeof in) != 0) {
        c2_log("nettest: bind 127.0.0.1:%d failed (%d)", g_port_base + c2_unit, WSAGetLastError());
    }
    g_ipx = s;
    c2_log("nettest: IPX socket -> udp 127.0.0.1:%d", g_port_base + c2_unit);
    return s;
}

static int WINAPI hk_bind(SOCKET s, const struct sockaddr *addr, int len)
{
    if (s == g_ipx) {
        g_ipx_socket = ((const ipx_addr *)addr)->sa_socket;
        return 0;
    }
    return real_bind(s, addr, len);
}

static int WINAPI hk_getsockname(SOCKET s, struct sockaddr *addr, int *len)
{
    if (s == g_ipx) {
        fill_ipx((ipx_addr *)addr, c2_unit);
        *len = 14;
        return 0;
    }
    return real_getsockname(s, addr, len);
}

static int WINAPI hk_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    const ipx_addr *ipx = (const ipx_addr *)to;
    struct sockaddr_in in;
    int u;
    if (s != g_ipx) {
        return real_sendto(s, buf, len, flags, to, tolen);
    }
    if (memcmp(ipx->sa_nodenum, "\xff\xff\xff\xff\xff\xff", 6) == 0) {
        for (u = 1; u <= MAX_UNITS; u++) {
            if (u != c2_unit) {
                in = udp_addr(u);
                real_sendto(s, buf, len, flags, (struct sockaddr *)&in, sizeof in);
            }
        }
        return len;
    }
    u = ipx->sa_nodenum[5];
    if (u < 1 || u > MAX_UNITS) {
        return len;    /* not one of ours: drop, as a real network would */
    }
    in = udp_addr(u);
    real_sendto(s, buf, len, flags, (struct sockaddr *)&in, sizeof in);
    return len;
}

static int WINAPI hk_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    struct sockaddr_in in;
    int inlen = sizeof in, n, unit;
    if (s != g_ipx) {
        return real_recvfrom(s, buf, len, flags, from, fromlen);
    }
    for (;;) {
        n = real_recvfrom(s, buf, len, flags, (struct sockaddr *)&in, &inlen);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET) {
            continue;   /* ICMP port unreachable from a unit that isn't running */
        }
        break;
    }
    if (n == SOCKET_ERROR) {
        return n;
    }
    unit = ntohs(in.sin_port) - g_port_base;
    if (from) {
        fill_ipx((ipx_addr *)from, unit);
    }
    if (fromlen) {
        *fromlen = 14;
    }
    return n;
}

static int WINAPI hk_closesocket(SOCKET s)
{
    if (s == g_ipx) {
        g_ipx = INVALID_SOCKET;
    }
    return real_closesocket(s);
}

static HWND WINAPI hk_GetForegroundWindow(void)
{
    return *(HWND *)0x6ad4c8;   /* gHWnd: every instance believes it has the foreground */
}

static BOOL WINAPI hk_SetForegroundWindow(HWND hwnd)
{
    (void)hwnd;
    return TRUE;
}

static HWND WINAPI hk_FindWindowA(LPCSTR cls, LPCSTR title)
{
    (void)cls;
    (void)title;
    return NULL;
}

static LRESULT CALLBACK test_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ACTIVATEAPP:
        if (!wp) {
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            return 0;
        }
        break;
    }
    return CallWindowProcA(g_game_wndproc, hwnd, msg, wp, lp);
}

static ATOM WINAPI hk_RegisterClassA(const WNDCLASSA *wc)
{
    WNDCLASSA copy = *wc;
    g_game_wndproc = wc->lpfnWndProc;
    copy.lpfnWndProc = test_wndproc;
    return real_RegisterClassA(&copy);
}

static BOOL WINAPI hk_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    /* tile the game windows (640x480) across the primary screen, in this process's (possibly
       DPI-virtualised) coordinates */
    if (!(flags & SWP_NOMOVE)) {
        int i = c2_unit - 1;
        int w = cx > 0 ? cx : 640, h = cy > 0 ? cy : 480;
        int cols = GetSystemMetrics(SM_CXSCREEN) / w;
        if (cols < 1) {
            cols = 1;
        }
        x = (i % cols) * w;
        y = (i / cols) * h;
    }
    return real_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    c2_log("CRASH: exception %08lx at %08lx (access %s %08lx) eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx "
           "esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
           ep->ExceptionRecord->ExceptionCode, (unsigned long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress,
           ep->ExceptionRecord->NumberParameters >= 2 && ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
           ep->ExceptionRecord->NumberParameters >= 2 ? (unsigned long)ep->ExceptionRecord->ExceptionInformation[1] : 0,
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    TerminateProcess(GetCurrentProcess(), 0xc2c2);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hk_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    (void)f;   /* keep crash_filter in place */
    return NULL;
}

void c2_nettest_install(void)
{
    char buf[32];
    if (!GetEnvironmentVariableA("C2_UNIT", buf, sizeof buf)) {
        return;
    }
    c2_unit = atoi(buf);
    if (c2_unit < 1 || c2_unit > MAX_UNITS) {
        c2_unit = 0;
        return;
    }
    if (GetEnvironmentVariableA("C2_PORT_BASE", buf, sizeof buf)) {
        g_port_base = atoi(buf);
    }
    c2_log("nettest: unit %d, udp base %d", c2_unit, g_port_base);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crash_filter);
    c2_hook_import("KERNEL32.dll", "SetUnhandledExceptionFilter", hk_SetUnhandledExceptionFilter);

    real_socket = c2_hook_import("WSOCK32.dll", "socket", hk_socket);
    real_bind = c2_hook_import("WSOCK32.dll", "bind", hk_bind);
    real_getsockname = c2_hook_import("WSOCK32.dll", "getsockname", hk_getsockname);
    real_sendto = c2_hook_import("WSOCK32.dll", "sendto", hk_sendto);
    real_recvfrom = c2_hook_import("WSOCK32.dll", "recvfrom", hk_recvfrom);
    real_closesocket = c2_hook_import("WSOCK32.dll", "closesocket", hk_closesocket);
    c2_probe_install();
    c2_hook_import("USER32.dll", "FindWindowA", hk_FindWindowA);
    c2_hook_import("USER32.dll", "GetForegroundWindow", hk_GetForegroundWindow);
    c2_hook_import("USER32.dll", "SetForegroundWindow", hk_SetForegroundWindow);
    real_RegisterClassA = c2_hook_import("USER32.dll", "RegisterClassA", hk_RegisterClassA);
    {
        char dir[MAX_PATH], *slash;
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        slash = strrchr(dir, '\\');
        if (slash) {
            slash[1] = '\0';
        }
        c2_nettest_input_start(dir);
    }
    if (!GetEnvironmentVariableA("C2_TILE", buf, sizeof buf) || atoi(buf)) {
        real_SetWindowPos = c2_hook_import("USER32.dll", "SetWindowPos", hk_SetWindowPos);
    }
}
