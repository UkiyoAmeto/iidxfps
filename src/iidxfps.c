/*
 * iidxfps - beatmania IIDX high precision frame timer (spice2x module)
 * Copyright (C) 2026  iidxfps contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 * The module is loaded by spice2x (https://github.com/spice2x/spice2x.github.io)
 * and uses its plugin SDK header (src/spicesdk.h, GPL-3.0).
 * It does NOT patch or modify bm2dx.dll in any way.
 * ---------------------------------------------------------------------------
 */
/*
 * iidxfps - beatmania IIDX high precision frame timer / monitor check helper
 *
 * Loaded as a spice2x module:   spice64.exe ... -k iidxfps.dll
 *
 * What it does (nothing on disk is modified, bm2dx.dll itself is never patched):
 *
 *   1) QPC frame pacer
 *      The game advances its logic once per presented frame, so the game speed
 *      follows the display refresh rate (a 144Hz display makes everything run
 *      20% fast).  We intercept IDirect3D9::CreateDevice(Ex) through the game's
 *      own import table, force the swap chain to D3DPRESENT_INTERVAL_IMMEDIATE
 *      (no vsync) and then gate IDirect3DDevice9::Present on a
 *      QueryPerformanceCounter schedule locked to the cabinet's native rate
 *      (120Hz for TDJ, 60Hz for LDJ).  Result: exactly N logic ticks per second
 *      no matter what the monitor refresh rate is.
 *
 *   2) Adapter refresh spoof
 *      bm2dx.dll imports EnumDisplaySettingsA and reads the adapter refresh
 *      rate out of the returned DEVMODE.  We hook that import so the game is
 *      always told the native rate (120/60), keeping its internal timing and
 *      its monitor check consistent with what we actually deliver.
 *
 *   3) Diagnostics
 *      Measured frame rate, late frames, skipped phases and the display mode the
 *      game asked for are written to the spice log, so the behaviour can be
 *      checked without a debugger.
 *
 * All hooks are import-table or per-object vtable hooks, i.e. they only depend
 * on *names* (Direct3DCreate9Ex, IDirect3D9::CreateDevice, Present,
 * EnumDisplaySettingsA), never on addresses inside a specific bm2dx.dll build.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "spicesdk.h"

/* ------------------------------------------------------------------ */
/* constants                                                          */
/* ------------------------------------------------------------------ */

/*
 * iidxfps - lock the game loop to the cabinet's native frame rate with
 * QueryPerformanceCounter, with vsync disabled so that the rate is exact on
 * any display.  The loop is held back *before* the real present, which is the
 * cheapest and most latency friendly place for a self paced frame limiter.
 */
#define MODULE_TAG  "iidxfps"
#define INI_NAME    "iidxfps.ini"
#define REPORT_NAME "iidxfps_report.txt"
#define IIDXFPS_VERSION "1.0.4"

#define DEFAULT_FPS_TDJ 120
#define DEFAULT_FPS_LDJ 60

#define PRESENT_INTERVAL_IMMEDIATE 0x80000000u

#define VTBL_IDIRECT3D9_CREATEDEVICE     16
#define VTBL_IDIRECT3D9_CREATEDEVICEEX   20
#define VTBL_DEVICE_GETSWAPCHAIN         14
#define VTBL_DEVICE_PRESENT              17
#define VTBL_DEVICE_PRESENTEX            121
#define VTBL_SWAPCHAIN_PRESENT           3
#define VTBL_RELEASE                     2

/* where the loop is actually held back; "auto" picks the one that is really
   being called, preferring the spice draw callback (once per game frame) */
#define PACE_AUTO       0
#define PACE_SWAPCHAIN  1
#define PACE_DEVICE     2
#define PACE_DRAW       3

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* ------------------------------------------------------------------ */
/* D3DPRESENT_PARAMETERS (hand rolled, no d3d9 headers needed)        */
/* ------------------------------------------------------------------ */

typedef struct {
    UINT  BackBufferWidth;
    UINT  BackBufferHeight;
    UINT  BackBufferFormat;
    UINT  BackBufferCount;
    UINT  MultiSampleType;
    DWORD MultiSampleQuality;
    UINT  SwapEffect;
    HWND  hDeviceWindow;
    BOOL  Windowed;
    BOOL  EnableAutoDepthStencil;
    UINT  AutoDepthStencilFormat;
    DWORD Flags;
    UINT  FullScreen_RefreshRateInHz;
    UINT  PresentationInterval;
} d3dpp_safe;

typedef char d3dpp_size_assert[(sizeof(d3dpp_safe) == 64) ? 1 : -1];
typedef char d3dpp_interval_assert[(offsetof(d3dpp_safe, PresentationInterval) == 60) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* types                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int enabled;
    int target_fps;          /* 0 = auto */
    int pace_at;             /* 0 = present, 1 = draw */
    int force_immediate;
    char spice_cfg_path[MAX_PATH];
    int spoof_refresh;
    int spoof_all_modes;
    int log_fps;
    int measure_only;
    int hook_present;
    int hook_presentex;
    int high_precision_time;
    int skip_when_match;
    int force_pacing;
} config_t;

typedef HRESULT (STDMETHODCALLTYPE *create_device_t)(
    void *, UINT, UINT, HWND, DWORD, void *, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_device_ex_t)(
    void *, UINT, UINT, HWND, DWORD, void *, void *, void **);
typedef HRESULT (STDMETHODCALLTYPE *present_t)(
    void *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (STDMETHODCALLTYPE *present_ex_t)(
    void *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *swapchain_present_t)(
    void *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *get_swapchain_t)(void *, UINT, void **);
typedef ULONG (STDMETHODCALLTYPE *release_t)(void *);
typedef HRESULT (WINAPI *d3dcreate9ex_t)(UINT, void **);
typedef void * (WINAPI *d3dcreate9_t)(UINT);
typedef BOOL (WINAPI *eds_a_t)(LPCSTR, DWORD, DEVMODEA *);
typedef BOOL (WINAPI *eds_w_t)(LPCWSTR, DWORD, DEVMODEW *);
typedef BOOL (WINAPI *eds_exa_t)(LPCSTR, DWORD, DEVMODEA *, DWORD);
typedef BOOL (WINAPI *eds_exw_t)(LPCWSTR, DWORD, DEVMODEW *, DWORD);

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */

static config_t         g_cfg;
static SPICE_SDK_V0     g_spice;
static CRITICAL_SECTION g_log_lock;
static int              g_log_lock_ready;

static HMODULE          g_game_module;
static volatile LONG    g_target_fps;
static volatile LONG    g_pacing_enabled;
static volatile LONG    g_measure_only;
static volatile LONG    g_device_hooked;
static volatile LONG    g_pace_point;      /* one of PACE_* */
static volatile LONG    g_spoof_on;
static void            *g_paced_device;
static volatile LONG    g_draw_seen;
static volatile LONG    g_fallback_logged;
static volatile LONG    g_halfspeed_logged;

static LARGE_INTEGER    g_qpf;
static LARGE_INTEGER    g_qpc_epoch;
static double           g_period_ticks;
static LONGLONG         g_deadline;
static LONGLONG         g_last_paced;
static int              g_pacer_started;
static HANDLE           g_wait_timer;

static volatile LONG64  g_ev_present;
static volatile LONG64  g_ev_swapchain;
static volatile LONG64  g_ev_draw;
static volatile LONG64  g_ev_paced;
static volatile LONG64  g_ev_late;
static volatile LONG64  g_ev_skipped;
static volatile LONG64  g_ev_last_qpc;
static volatile LONG64  g_ev_min_gap;
static volatile LONG64  g_ev_max_gap;

static void *g_real_createdevice;
static void *g_real_createdeviceex;
static void *g_real_present;
static void *g_real_presentex;
static void *g_real_sc_present;
static void *g_real_d3dcreate9ex;
static void *g_real_d3dcreate9;
static void *g_real_edsa;
static void *g_real_edsw;
static void *g_real_edsexa;
static void *g_real_edsexw;
static void *g_real_timegettime;
static void *g_real_gettickcount;
static void *g_real_gettickcount64;

/* forward declarations */
static HRESULT STDMETHODCALLTYPE present_hook(
    void *, const RECT *, const RECT *, HWND, const RGNDATA *);
static HRESULT STDMETHODCALLTYPE presentex_hook(
    void *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
static HRESULT STDMETHODCALLTYPE createdevice_hook(
    void *, UINT, UINT, HWND, DWORD, void *, void **);
static HRESULT STDMETHODCALLTYPE createdeviceex_hook(
    void *, UINT, UINT, HWND, DWORD, void *, void *, void **);
static void do_install_device_hooks(void *device);
static const char *pace_point_name(void);

/* ------------------------------------------------------------------ */
/* logging                                                            */
/* ------------------------------------------------------------------ */

/* ring buffer of the most recent messages, so that the report file a player
   sends back is self contained */
#define LOG_RING_SIZE 24
#define LOG_RING_LEN 200
static char g_log_ring[LOG_RING_SIZE][LOG_RING_LEN];
static volatile LONG g_log_ring_next;

static void log_ring_add(const char *text) {
    LONG slot = InterlockedIncrement(&g_log_ring_next) - 1;
    char *dst = g_log_ring[slot % LOG_RING_SIZE];
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(dst, LOG_RING_LEN - 1,
              "%02d:%02d:%02d %s", st.wHour, st.wMinute, st.wSecond, text);
    dst[LOG_RING_LEN - 1] = '\0';
}

static void logmsg(int level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    if (g_log_lock_ready) {
        EnterCriticalSection(&g_log_lock);
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    log_ring_add(buf);
    if (g_spice.log) {
        g_spice.log((SPICE_SDK_LOG_LEVEL)level, MODULE_TAG, buf);
    }
    if (g_log_lock_ready) {
        LeaveCriticalSection(&g_log_lock);
    }
}

#define LOG_MISC(...)  logmsg(SPICE_SDK_LOG_LEVEL_MISC, __VA_ARGS__)
#define LOG_INFO(...)  logmsg(SPICE_SDK_LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_WARN(...)  logmsg(SPICE_SDK_LOG_LEVEL_WARNING, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* config                                                             */
/* ------------------------------------------------------------------ */

static void config_init(const char *ini_path) {
    char buf[64];

    g_cfg.enabled         = GetPrivateProfileIntA("General", "Enabled", 1, ini_path);
    g_cfg.force_immediate = GetPrivateProfileIntA("General", "ForceImmediatePresent", 1, ini_path);
    g_cfg.spoof_refresh   = GetPrivateProfileIntA("General", "SpoofDisplayRefresh", 1, ini_path);
    GetPrivateProfileStringA("General", "SpiceCfgPath", "", g_cfg.spice_cfg_path,
                             sizeof(g_cfg.spice_cfg_path) - 1, ini_path);
    g_cfg.spoof_all_modes = GetPrivateProfileIntA("General", "SpoofAllModes", 1, ini_path);
    g_cfg.log_fps         = GetPrivateProfileIntA("General", "LogFrameRate", 1, ini_path);
    g_cfg.measure_only    = GetPrivateProfileIntA("General", "MeasureOnly", 0, ini_path);
    g_cfg.hook_present    = GetPrivateProfileIntA("General", "HookPresent", 1, ini_path);
    g_cfg.hook_presentex  = GetPrivateProfileIntA("General", "HookPresentEx", 1, ini_path);
    g_cfg.high_precision_time = GetPrivateProfileIntA("General", "HighPrecisionTime", 1, ini_path);
    g_cfg.skip_when_match = GetPrivateProfileIntA("General", "SkipWhenRefreshMatches", 1, ini_path);
    g_cfg.force_pacing    = GetPrivateProfileIntA("General", "ForcePacing", 0, ini_path);


    buf[0] = '\0';
    GetPrivateProfileStringA("General", "FPS", "auto", buf, sizeof(buf) - 1, ini_path);
    if (_stricmp(buf, "auto") == 0 || buf[0] == '\0') {
        g_cfg.target_fps = 0;
    } else {
        g_cfg.target_fps = atoi(buf);
        if (g_cfg.target_fps < 1 || g_cfg.target_fps > 1000) {
            g_cfg.target_fps = 0;
        }
    }

    buf[0] = '\0';
    GetPrivateProfileStringA("General", "PaceAt", "auto", buf, sizeof(buf) - 1, ini_path);
    if (_stricmp(buf, "draw") == 0) {
        g_cfg.pace_at = PACE_DRAW;
    } else if (_stricmp(buf, "present") == 0) {
        g_cfg.pace_at = PACE_DEVICE;
    } else if (_stricmp(buf, "swapchain") == 0) {
        g_cfg.pace_at = PACE_SWAPCHAIN;
    } else {
        g_cfg.pace_at = PACE_AUTO;
    }
}

/* ------------------------------------------------------------------ */
/* target rate detection                                              */
/* ------------------------------------------------------------------ */

static int file_equals(const char *path, const char *expect) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char buf[64];
    DWORD read = 0;
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    memset(buf, 0, sizeof(buf));
    ReadFile(f, buf, sizeof(buf) - 1, &read, NULL);
    CloseHandle(f);
    while (read > 0 && (buf[read - 1] == '\n' || buf[read - 1] == '\r' ||
                        buf[read - 1] == ' ' || buf[read - 1] == '\t')) {
        buf[--read] = '\0';
    }
    return _stricmp(buf, expect) == 0;
}

/* spicecfg (the GUI) stores its options in %APPDATA%\spicetools.xml, so a TDJ
   cabinet enabled there is another instant signal - no rom probe needed, and the
   decision is made before the game enumerates a single display mode.
   spice2x treats a bool option as enabled when its value is *non-empty*
   (cfg/option.cpp: value_bool() { return !value.empty(); }), so
       <option name="iidxtdj" value="1"/>   means enabled while
       <option name="iidxtdj" value=""/>    means disabled.
   The file also holds the user's card id and other private settings, so nothing
   but the option name is ever logged. */
static int buffer_has_tdj_option(const char *buf, size_t len) {
    static const char name_attr[] = "name=\"";
    static const char value_attr[] = "value=\"";
    size_t i = 0;

    while (i + sizeof(name_attr) - 1 <= len) {
        size_t nstart, nend;

        if (memcmp(buf + i, name_attr, sizeof(name_attr) - 1) != 0) {
            i++;
            continue;
        }
        nstart = i + sizeof(name_attr) - 1;
        nend = nstart;
        while (nend < len && buf[nend] != '"' && buf[nend] != '>') {
            nend++;
        }
        if (nend - nstart < 7) {                       /* too short for iidxtdj */
            i = nend;
            continue;
        }
        {
            size_t namelen = nend - nstart;
            /* only the mode switches count: iidxtdj / iidxtdjw / sp2x-iidxtdjw.
               Settings that merely mention tdj (iidxtdjcamhook, iidxtdjsubsize)
               persist after TDJ is switched off, so they must not match. */
            int is_switch = 0;
            char name[64];
            if (namelen < sizeof(name)) {
                memcpy(name, buf + nstart, namelen);
                name[namelen] = '\0';
                is_switch = _stricmp(name, "iidxtdj") == 0 ||
                            _stricmp(name, "iidxtdjw") == 0 ||
                            _stricmp(name, "sp2x-iidxtdjw") == 0;
            }
            if (!is_switch) {
                i = nend;
                continue;
            }
        }
        /* look for the value attribute of the same element */
        {
            size_t v = nend;
            size_t limit = v + 512;
            if (limit > len) {
                limit = len;
            }
            while (v + sizeof(value_attr) - 1 <= limit) {
                if (memcmp(buf + v, value_attr, sizeof(value_attr) - 1) == 0) {
                    size_t vend = v + sizeof(value_attr) - 1;
                    if (vend < len && buf[vend] != '"') {
                        return 1;                      /* non-empty: enabled */
                    }
                    break;
                }
                if (buf[v] == '>') {
                    break;                             /* element ended, no value */
                }
                v++;
            }
        }
        i = nend;
    }
    return 0;
}

/* the file is a few hundred kB, so it is read in chunks; the last TAIL bytes of
   every chunk are carried over so an element split across two reads is still
   matched.  The carried length must always stay below the buffer size, otherwise
   ReadFile is asked to write past it (and the memmove below would read past it). */
#define SPICECFG_BUF_SIZE 16384
#define SPICECFG_TAIL     4096

static int spicecfg_says_tdj(const char *path) {
    HANDLE f;
    char buf[SPICECFG_BUF_SIZE];
    DWORD got = 0;
    size_t carry = 0;
    int found = 0;

    if (!path || !path[0]) {
        return 0;
    }
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    for (;;) {
        DWORD want = (DWORD)(sizeof(buf) - carry - 1);
        size_t total;

        if (want == 0 || !ReadFile(f, buf + carry, want, &got, NULL) || got == 0) {
            break;
        }
        total = carry + got;
        buf[total] = '\0';
        if (buffer_has_tdj_option(buf, total)) {
            found = 1;
            break;
        }
        if (total > SPICECFG_TAIL) {
            memmove(buf, buf + total - SPICECFG_TAIL, SPICECFG_TAIL);
            carry = SPICECFG_TAIL;
        } else {
            carry = total;
        }
    }
    CloseHandle(f);
    return found;
}

static int spicecfg_config_says_tdj(void) {
    char path[MAX_PATH];

    if (g_cfg.spice_cfg_path[0]) {
        return spicecfg_says_tdj(g_cfg.spice_cfg_path);
    }
    path[0] = '\0';
    if (GetEnvironmentVariableA("APPDATA", path, sizeof(path)) == 0 ||
        strlen(path) + 16 >= sizeof(path)) {
        return 0;
    }
    strcat(path, "\\spicetools.xml");
    return spicecfg_says_tdj(path);
}

/* Every TDJ related spice2x option contains "iidxtdj" (-iidxtdj, -iidxtdjw,
   -iidxtdjsubsize, -iidxtdjcamhook...), and none of them exists for the LDJ
   cabinet, so the launcher command line is an exact *and instant* answer.  It is
   read before the game enumerates a single display mode, which keeps the whole
   mode table and the refresh spoof consistent from the first frame on. */
static int command_line_says_tdj(void) {
    /* only the actual mode switches count: -iidxtdj / -iidxtdjw / -sp2x-iidxtdjw.
       Options that merely start with it (-iidxtdjcamhook, -iidxtdjsubsize) can be
       set while the cabinet is still LDJ, so they must not match.  An option name
       starts right after - or / and ends at a delimiter. */
    static const char *names[] = { "iidxtdjw", "iidxtdj", NULL };
    const char *cmd = GetCommandLineA();
    int n;

    if (!cmd) {
        return 0;
    }
    for (n = 0; names[n]; n++) {
        const char *p = cmd;
        size_t len = strlen(names[n]);
        while ((p = strstr(p, names[n])) != NULL) {
            char before = (p == cmd) ? ' ' : p[-1];
            char after = p[len];
            int ok_before = (before == '-' || before == '/');
            int ok_after = (after == '\0' || after == ' ' || after == '\t' ||
                            after == '=' || after == '"');
            if (ok_before && ok_after) {
                return 1;
            }
            p++;
        }
    }
    return 0;
}

/* same detection spice2x itself uses for the TDJ cabinet */
static int detect_tdj(void) {
    if (command_line_says_tdj()) {
        LOG_INFO("TDJ mode: the launcher command line contains an iidxtdj option");
        return 1;
    }
    if (spicecfg_config_says_tdj()) {
        LOG_INFO("TDJ mode: spicecfg's options file has an iidxtdj option enabled");
        return 1;
    }
    if (file_equals("C:\\000rom.txt", "TDJ-JA")) {
        return 1;
    }
    if (file_equals("D:\\001rom.txt", "TDJ")) {
        return 1;
    }
    /* NOTE: the ea3 model string is *not* used here.  In TDJ mode spice2x only
       injects the cabinet rom in memory, the game's prop/ea3-config.xml keeps
       saying "LDJ", so checking the model would never match a real TDJ setup
       (and would misfire on a mis-configured install).  The cabinet rom is
       checked at runtime instead, see avs_read_cabinet_rom(). */
    return 0;
}

/* ------------------------------------------------------------------ */
/* TDJ detection through the AVS filesystem                           */
/* ------------------------------------------------------------------ */
/*
 * spice2x switches a cabinet to TDJ by injecting the cabinet rom in memory:
 *
 *     hooks::avs::set_rom("/c_drv/000rom.txt", "TDJ-JA");
 *
 * so the game's own read of that path is answered by spice2x's avs_fs_open
 * hook and no file exists on disk.  The only way for a module to see the same
 * truth is to ask the very same AVS filesystem, i.e. call avs2-core.dll.
 *
 * avs2-core.dll exports under obfuscated names that change between builds, so
 * every name set spice2x knows about is tried (taken from spice2x avs/core.cpp)
 * which keeps this independent of the game and core version.
 */
typedef unsigned int avs_file_t;

typedef struct {
    const char *open;
    const char *read;
    const char *close;
} avs_name_set_t;

static const avs_name_set_t AVS_NAME_SETS[] = {
    { "avs_fs_open",    "avs_fs_read",    "avs_fs_close"    },
    { "XCgsqzn000004e", "XCgsqzn0000051", "XCgsqzn0000055" },
    { "XCnbrep700004e", "XCnbrep7000051", "XCnbrep7000055" },
    { "XCnbrep7000039", "XCnbrep700003c", "XCnbrep7000040" },
    { "XCd229cc000090", "XCd229cc00010d", "XCd229cc00011f" },
    { "XC058ba50000b6", "XC058ba5000139", "XC058ba500011b" },
    { "XC0bbe970000b3", "XC0bbe97000134", "XC0bbe97000119" },
};

typedef unsigned int (*avs_fs_open_t)(const char *name, unsigned short mode, int flags);
typedef unsigned int (*avs_fs_read_t)(avs_file_t file, unsigned char *data, unsigned int size);
typedef void         (*avs_fs_close_t)(avs_file_t file);

static avs_fs_open_t  g_avs_open;
static avs_fs_read_t  g_avs_read;
static avs_fs_close_t g_avs_close;
static int            g_avs_state;         /* 0 unknown, 1 ok, -1 unavailable */

static int avs_resolve(void) {
    HMODULE core;
    unsigned i;

    if (g_avs_state != 0) {
        return g_avs_state > 0;
    }
    core = GetModuleHandleA("avs2-core.dll");
    if (!core) {
        return 0;                          /* not loaded yet, try again later */
    }
    for (i = 0; i < sizeof(AVS_NAME_SETS) / sizeof(AVS_NAME_SETS[0]); i++) {
        const avs_name_set_t *s = &AVS_NAME_SETS[i];
        avs_fs_open_t  o = (avs_fs_open_t)(void *)GetProcAddress(core, s->open);
        avs_fs_read_t  r = (avs_fs_read_t)(void *)GetProcAddress(core, s->read);
        avs_fs_close_t c = (avs_fs_close_t)(void *)GetProcAddress(core, s->close);
        if (o && r && c) {
            g_avs_open = o;
            g_avs_read = r;
            g_avs_close = c;
            g_avs_state = 1;
            LOG_INFO("avs2-core.dll filesystem functions resolved via '%s'", s->open);
            return 1;
        }
    }
    g_avs_state = -1;
    LOG_INFO("avs2-core.dll is loaded but its filesystem exports could not be resolved; "
             "TDJ auto detection falls back to the rom files");
    return 0;
}

/* reads the cabinet rom the game itself reads; returns the length, 0 on failure.
 *
 * The descriptor is validated before anything else: a failed avs_fs_open does
 * *not* return 0, it returns an HRESULT style error (0x80070002 for a missing
 * file), and passing such a value to avs_fs_read/avs_fs_close corrupts the
 * game's filesystem state and crashes the process.  The read length is clamped
 * as well, because an error return from avs_fs_read would otherwise be treated
 * as a huge length. */
static int avs_desc_valid(avs_file_t f) {
    return f != 0 && f != 0xFFFFFFFFu && (f & 0x80000000u) == 0;
}

static int avs_read_cabinet_rom(char *buf, size_t bufsz) {
    avs_file_t f;
    size_t n;

    if (bufsz < 2 || !avs_resolve()) {
        return 0;
    }
    f = g_avs_open("/c_drv/000rom.txt", 1, 0);       /* mode 1 = read */
    if (!avs_desc_valid(f)) {
        return 0;
    }
    memset(buf, 0, bufsz);
    n = g_avs_read(f, (unsigned char *)buf, (unsigned int)(bufsz - 1));
    g_avs_close(f);
    if (n >= bufsz) {
        n = bufsz - 1;                            /* never trust the length */
    }
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' ||
                     buf[n - 1] == ' '  || buf[n - 1] == '\t')) {
        buf[--n] = '\0';
    }
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* runtime target rate change                                         */
/* ------------------------------------------------------------------ */

static int g_desktop_refresh;   /* defined further down */
static char g_refresh_fit[128];  /* "144Hz / 120 fps = 1.20 refreshes per frame" */
static char g_refresh_advice[256];
static void decide_pacing(void);
static void analyse_refresh_fit(void);

static void set_target_fps(int fps, const char *why) {
    int old = (int)g_target_fps;

    if (fps <= 0 || fps == old) {
        return;
    }
    g_target_fps = fps;
    g_period_ticks = (double)g_qpf.QuadPart / (double)fps;
    g_pacer_started = 0;                   /* re-arm the schedule */
    LOG_INFO("target frame rate %d -> %d fps (%s)", old, fps, why);
    analyse_refresh_fit();
}

/* Called once the game is up and running (from the game's own thread), because
   TDJ mode is only visible through the cabinet rom the game reads at boot.
   Returns 1 when the cabinet rom was read (contents in buf). */
static int rom_probe_attempts;
static char g_rom_content[64];
static volatile LONG g_rom_probe_done;

static void tdj_recheck_from_rom(void) {
    char buf[64];

    if (InterlockedCompareExchange(&g_rom_probe_done, 1, 0) != 0) {
        return;
    }
    if (avs_read_cabinet_rom(buf, sizeof(buf))) {
        memset(g_rom_content, 0, sizeof(g_rom_content));
        strncpy(g_rom_content, buf, sizeof(g_rom_content) - 1);
        LOG_INFO("cabinet rom (/c_drv/000rom.txt) = \"%s\"", buf);
        if (g_cfg.target_fps != 0) {
            return;                        /* the user pinned the rate in the ini */
        }
        if (_strnicmp(buf, "TDJ", 3) == 0) {
            if (g_target_fps != DEFAULT_FPS_TDJ) {
                set_target_fps(DEFAULT_FPS_TDJ, "the game's cabinet rom says TDJ");
                if (g_paced_device && !g_pacing_enabled) {
                    /* the device already exists, so vsync can no longer be turned
                       off and the loop stays capped by the display refresh rate */
                    LOG_WARN("this is a TDJ (120 fps) cabinet but the module is hands-off "
                             "because the desktop runs at %dHz; set FPS=120 in " INI_NAME
                             " and restart the game, otherwise the game runs at half speed "
                             "and the monitor check will fail", g_desktop_refresh);
                }
            }
        } else if (_strnicmp(buf, "LDJ", 3) == 0 && g_target_fps != DEFAULT_FPS_LDJ) {
            set_target_fps(DEFAULT_FPS_LDJ, "the game's cabinet rom says LDJ");
        }
        return;
    }
    /* the rom could not be read: retry a few times, then accept "not a TDJ
       cabinet" (on an LDJ cabinet the path simply does not exist) */
    if (++rom_probe_attempts < 10) {
        InterlockedExchange(&g_rom_probe_done, 0);
        return;
    }
    LOG_INFO("cabinet rom (/c_drv/000rom.txt) is not readable "
             "(normal on an LDJ cabinet)");
}

static void resolve_target_fps(void) {
    int fps;
    if (g_cfg.target_fps > 0) {
        fps = g_cfg.target_fps;
        LOG_INFO("target frame rate: %d fps (set in " INI_NAME ")", fps);
    } else {
        int tdj = detect_tdj();
        fps = tdj ? DEFAULT_FPS_TDJ : DEFAULT_FPS_LDJ;
        LOG_INFO("target frame rate: %d fps (auto: %s)", fps,
                 tdj ? "TDJ cabinet (120 fps timing)"
                     : "LDJ cabinet, will be confirmed against the game's cabinet rom");
    }
    g_target_fps = fps;
    g_period_ticks = (double)g_qpf.QuadPart / (double)fps;
}

/* ------------------------------------------------------------------ */
/* high precision wait                                                */
/* ------------------------------------------------------------------ */

static void pacer_global_init(void) {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc_epoch);
    g_wait_timer = CreateWaitableTimerExW(
        NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_wait_timer) {
        g_wait_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    LOG_INFO("qpc frequency %lld, wait timer %s",
             (long long)g_qpf.QuadPart,
             g_wait_timer ? "ok" : "unavailable (falling back to Sleep)");
}

static void wait_until(LONGLONG target) {
    for (;;) {
        LARGE_INTEGER now;
        LONGLONG remain;
        double remain_ms;
        QueryPerformanceCounter(&now);
        remain = target - now.QuadPart;
        if (remain <= 0) {
            return;
        }
        remain_ms = (double)remain * 1000.0 / (double)g_qpf.QuadPart;
        if (remain_ms > 2.0) {
            if (g_wait_timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((remain_ms - 1.0) * 10000.0);
                if (SetWaitableTimer(g_wait_timer, &due, 0, NULL, NULL, FALSE)) {
                    WaitForSingleObject(g_wait_timer, (DWORD)remain_ms);
                    continue;
                }
            }
            Sleep((DWORD)(remain_ms - 1.0));
        } else {
            YieldProcessor();
        }
    }
}

/* ------------------------------------------------------------------ */
/* the pacer                                                          */
/* ------------------------------------------------------------------ */

static void record_frame_gap(void);

static void pace_frame(void) {
    LARGE_INTEGER now;
    LONGLONG period;

    if (!g_pacing_enabled || g_measure_only) {
        return;
    }
    /* Frame interval telemetry.  The spice draw callback is the natural place
       (exactly one call per game frame) and owns the measurement whenever it
       exists; the pacing call only records for spice builds without
       register_d3d9, where the draw callback never fires.  Recording in both
       would count every frame twice and halve the reported gaps. */
    if (!InterlockedCompareExchange(&g_draw_seen, 0, 0)) {
        record_frame_gap();
    }
    period = (LONGLONG)(g_period_ticks + 0.5);
    if (period <= 0) {
        return;
    }

    if (!g_pacer_started) {
        QueryPerformanceCounter(&now);
        g_pacer_started = 1;
        g_deadline = now.QuadPart + period;
        g_last_paced = now.QuadPart;
        InterlockedIncrement64(&g_ev_paced);
        return;
    }

    QueryPerformanceCounter(&now);

    if (now.QuadPart < g_deadline) {
        /* this call owns the current frame: hold the loop here until the
           scheduled presentation time.  A loop that runs thousands of times per
           second is thereby slowed to exactly one period per call. */
        wait_until(g_deadline);
        QueryPerformanceCounter(&now);
        g_deadline += period;
        g_last_paced = now.QuadPart;
        InterlockedIncrement64(&g_ev_paced);
        return;
    }

    if (now.QuadPart - g_deadline > period * 2) {
        /* badly behind (asset load, alt-tab, ...): resync instead of catching
           up, catching up would make the game run faster than real time */
        InterlockedIncrement64(&g_ev_late);
        g_deadline = now.QuadPart + period;
    } else {
        g_deadline += period;
    }

    g_last_paced = now.QuadPart;
    if (g_deadline <= now.QuadPart) {
        g_deadline = now.QuadPart + period;
    }
    InterlockedIncrement64(&g_ev_paced);
}

/* ------------------------------------------------------------------ */
/* module footprint audit                                             */
/* ------------------------------------------------------------------ */
/*
 * The report lists every single change this module makes to the running game, so
 * that anyone can check the claim "judgement windows and note scroll speed cannot
 * be affected": the game's code is never modified.  Only import slots (function
 * pointers), shared COM vtable slots and the present parameters are touched, and
 * no game logic is patched, hooked or rewritten.
 */
#define MAX_AUDIT 40

typedef struct {
    char  what[72];
    void *slot;
    void *from;
    void *to;
} audit_entry_t;

static audit_entry_t g_audit[MAX_AUDIT];
static int g_audit_count;
static volatile LONG g_audit_lock;

static void audit_add(const char *what, void *slot, void *from, void *to) {
    while (InterlockedCompareExchange(&g_audit_lock, 1, 0) != 0) {
        Sleep(0);
    }
    if (g_audit_count < MAX_AUDIT) {
        int i;
        for (i = 0; i < (int)sizeof(g_audit[0].what) - 1 && what[i]; i++) {
            g_audit[g_audit_count].what[i] = what[i];
        }
        g_audit[g_audit_count].what[i] = '\0';
        g_audit[g_audit_count].slot = slot;
        g_audit[g_audit_count].from = from;
        g_audit[g_audit_count].to = to;
        g_audit_count++;
    }
    InterlockedExchange(&g_audit_lock, 0);
}

static const char *vtable_slot_name(int index) {
    switch (index) {
        case VTBL_IDIRECT3D9_CREATEDEVICE:   return "IDirect3D9::CreateDevice";
        case VTBL_IDIRECT3D9_CREATEDEVICEEX: return "IDirect3D9Ex::CreateDeviceEx";
        case VTBL_DEVICE_PRESENT:            return "IDirect3DDevice9::Present";
        case VTBL_DEVICE_PRESENTEX:          return "IDirect3DDevice9Ex::PresentEx";
        case VTBL_SWAPCHAIN_PRESENT:         return "IDirect3DSwapChain9::Present";
        default:                             return "COM vtable slot";
    }
}
static void audit_add_iat(void *slot, const char *dll, const char *func, void *from, void *to) {
    char what[72];
    int n = 0;
    const char *p = "IAT ";
    while (*p && n < (int)sizeof(what) - 1) what[n++] = *p++;
    p = dll;
    while (*p && n < (int)sizeof(what) - 1) what[n++] = *p++;
    if (n < (int)sizeof(what) - 1) what[n++] = '!';
    p = func;
    while (*p && n < (int)sizeof(what) - 1) what[n++] = *p++;
    what[n] = '\0';
    audit_add(what, slot, from, to);
}

static void audit_add_vtable(void *vtbl, int index, void *from, void *to) {
    char what[72];
    int n = 0;
    const char *p;
    (void)vtbl;
    p = "COM vtable slot (shared): ";
    while (*p && n < (int)sizeof(what) - 1) what[n++] = *p++;
    p = vtable_slot_name(index);
    while (*p && n < (int)sizeof(what) - 1) what[n++] = *p++;
    what[n] = '\0';
    audit_add(what, vtbl, from, to);
}
/* ------------------------------------------------------------------ */
/* IAT hooking                                                        */
/* ------------------------------------------------------------------ */

static int iat_hook(HMODULE mod, const char *dll_name, const char *func_name,
                    void *hook_fn, void **orig_out) {
    uint8_t *base;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *desc;
    int hooked = 0;

    if (!mod) {
        return 0;
    }
    base = (uint8_t *)mod;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 0;
    }
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir->VirtualAddress == 0) {
        return 0;
    }

    desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
    for (; desc->Name != 0; desc++) {
        const char *name = (const char *)(base + desc->Name);
        IMAGE_THUNK_DATA *oft;
        IMAGE_THUNK_DATA *ft;

        if (dll_name && dll_name[0] && _stricmp(name, dll_name) != 0) {
            continue;
        }
        oft = desc->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk)
            : (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);
        ft = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);

        for (; oft->u1.AddressOfData != 0; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            const char *fname;
            DWORD old_protect = 0;

            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                continue;
            }
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + (uint32_t)oft->u1.AddressOfData);
            fname = (const char *)ibn->Name;
            if (_stricmp(fname, func_name) != 0) {
                continue;
            }
            if (!VirtualProtect(&ft->u1.Function, sizeof(void *),
                                PAGE_EXECUTE_READWRITE, &old_protect)) {
                LOG_WARN("iat hook %s: VirtualProtect failed (%lu)", func_name, GetLastError());
                continue;
            }
            if (orig_out) {
                *orig_out = (void *)(uintptr_t)ft->u1.Function;
            }
            {
                void *old_value = (void *)(uintptr_t)ft->u1.Function;
                ft->u1.Function = (uintptr_t)hook_fn;
                VirtualProtect(&ft->u1.Function, sizeof(void *), old_protect, &old_protect);
                FlushInstructionCache(GetCurrentProcess(), &ft->u1.Function, sizeof(void *));
                audit_add_iat((void *)&ft->u1.Function, name, fname, old_value, hook_fn);
            }
            hooked++;
        }
    }
    return hooked;
}

/* Patch one slot of a *shared* COM vtable (the copy that lives in the owning
   module's read-only data).
 *
 * The obvious alternative - giving the object its own private copy of the
 * vtable and swapping the object's vtable pointer - is NOT safe here: the D3D9
 * runtime keeps metadata attached to the vtable (and caches the pointer to it
 * inside the device), so a private copy makes it look up garbage and it jumps
 * to NULL.  Patching the shared entry keeps everything consistent.
 *
 * Patching the same slot twice would capture our own hook as the "original" and
 * recurse forever, so already patched slots are remembered.
 */
#define MAX_VTABLE_PATCHES 16
static struct {
    void **vtbl;
    int index;
    void *orig;
} g_vtable_patches[MAX_VTABLE_PATCHES];
static int g_vtable_patch_count;
static CRITICAL_SECTION g_vtable_lock;

static void *vtable_patch(void *obj, int index, void *hook_fn) {
    void **vtbl;
    void *orig;
    DWORD old_protect = 0;
    int i;

    if (!obj) {
        return NULL;
    }
    EnterCriticalSection(&g_vtable_lock);

    vtbl = *(void ***)obj;
    if (!vtbl) {
        LeaveCriticalSection(&g_vtable_lock);
        return NULL;
    }
    for (i = 0; i < g_vtable_patch_count; i++) {
        if (g_vtable_patches[i].vtbl == vtbl && g_vtable_patches[i].index == index) {
            void *already = g_vtable_patches[i].orig;
            LeaveCriticalSection(&g_vtable_lock);
            return already;
        }
    }

    if (!VirtualProtect(&vtbl[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect)) {
        LOG_WARN("vtable patch index %d: VirtualProtect failed (%lu)", index, GetLastError());
        LeaveCriticalSection(&g_vtable_lock);
        return NULL;
    }
    orig = vtbl[index];
    vtbl[index] = hook_fn;
    VirtualProtect(&vtbl[index], sizeof(void *), old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[index], sizeof(void *));

    if (g_vtable_patch_count < MAX_VTABLE_PATCHES) {
        g_vtable_patches[g_vtable_patch_count].vtbl = vtbl;
        g_vtable_patches[g_vtable_patch_count].index = index;
        g_vtable_patches[g_vtable_patch_count].orig = orig;
        g_vtable_patch_count++;
        audit_add_vtable(vtbl, index, orig, hook_fn);
    }
    LeaveCriticalSection(&g_vtable_lock);
    return orig;
}

/* ------------------------------------------------------------------ */
/* CreateDevice / CreateDeviceEx hooks                                */
/* ------------------------------------------------------------------ */

static void log_present_params(const char *what, const d3dpp_safe *p) {
    if (!p) {
        return;
    }
    LOG_INFO("%s: %ux%u fmt=%u buffers=%u swap=%u windowed=%d refresh=%u interval=0x%08X flags=0x%X",
             what, p->BackBufferWidth, p->BackBufferHeight, p->BackBufferFormat,
             p->BackBufferCount, p->SwapEffect, (int)p->Windowed,
             p->FullScreen_RefreshRateInHz, p->PresentationInterval, p->Flags);
}

static HRESULT STDMETHODCALLTYPE createdevice_hook(
    void *self, UINT adapter, UINT device_type, HWND focus_window,
    DWORD behavior_flags, void *present_params, void **device_out) {
    d3dpp_safe *p = (d3dpp_safe *)present_params;
    UINT saved_interval = 0;
    int modified = 0;
    HRESULT hr;

    log_present_params("CreateDevice", p);

    /* Last chance to change the target rate and the vsync decision: the cabinet
       rom has been read by the game long before the device is created, and the
       boot time monitor check only runs afterwards. */
    tdj_recheck_from_rom();
    decide_pacing();

    if (p && g_cfg.force_immediate && g_pacing_enabled && !g_measure_only &&
        p->PresentationInterval != PRESENT_INTERVAL_IMMEDIATE) {
        saved_interval = p->PresentationInterval;
        p->PresentationInterval = PRESENT_INTERVAL_IMMEDIATE;
        modified = 1;
        LOG_INFO("CreateDevice: PresentationInterval 0x%08X -> IMMEDIATE (vsync off)",
                 saved_interval);
        audit_add("present params: PresentationInterval -> IMMEDIATE",
                  present_params, (void *)(uintptr_t)saved_interval,
                  (void *)(uintptr_t)PRESENT_INTERVAL_IMMEDIATE);
    }

    hr = ((create_device_t)g_real_createdevice)(
        self, adapter, device_type, focus_window, behavior_flags, present_params, device_out);

    if (FAILED(hr) && modified) {
        LOG_WARN("CreateDevice failed (0x%08X) with IMMEDIATE, retrying with the game's parameters", hr);
        p->PresentationInterval = saved_interval;
        hr = ((create_device_t)g_real_createdevice)(
            self, adapter, device_type, focus_window, behavior_flags, present_params, device_out);
    }

    if (SUCCEEDED(hr)) {
        LOG_INFO("CreateDevice ok (0x%08X) device=%p", hr, device_out ? *device_out : NULL);
        if (device_out) {
            do_install_device_hooks(*device_out);
        }
    } else {
        LOG_WARN("CreateDevice failed (0x%08X)", hr);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE createdeviceex_hook(
    void *self, UINT adapter, UINT device_type, HWND focus_window,
    DWORD behavior_flags, void *present_params, void *fullscreen_mode, void **device_out) {
    d3dpp_safe *p = (d3dpp_safe *)present_params;
    UINT saved_interval = 0;
    int modified = 0;
    HRESULT hr;

    log_present_params("CreateDeviceEx", p);

    /* see createdevice_hook: settle the cabinet type before deciding about vsync */
    tdj_recheck_from_rom();
    decide_pacing();

    if (p && g_cfg.force_immediate && g_pacing_enabled && !g_measure_only &&
        p->PresentationInterval != PRESENT_INTERVAL_IMMEDIATE) {
        saved_interval = p->PresentationInterval;
        p->PresentationInterval = PRESENT_INTERVAL_IMMEDIATE;
        modified = 1;
        LOG_INFO("CreateDeviceEx: PresentationInterval 0x%08X -> IMMEDIATE (vsync off)",
                 saved_interval);
        audit_add("present params: PresentationInterval -> IMMEDIATE",
                  present_params, (void *)(uintptr_t)saved_interval,
                  (void *)(uintptr_t)PRESENT_INTERVAL_IMMEDIATE);
    }

    hr = ((create_device_ex_t)g_real_createdeviceex)(
        self, adapter, device_type, focus_window, behavior_flags, present_params,
        fullscreen_mode, device_out);

    if (FAILED(hr) && modified) {
        LOG_WARN("CreateDeviceEx failed (0x%08X) with IMMEDIATE, retrying with the game's parameters", hr);
        p->PresentationInterval = saved_interval;
        hr = ((create_device_ex_t)g_real_createdeviceex)(
            self, adapter, device_type, focus_window, behavior_flags, present_params,
            fullscreen_mode, device_out);
    }

    if (SUCCEEDED(hr)) {
        LOG_INFO("CreateDeviceEx ok (0x%08X) device=%p", hr, device_out ? *device_out : NULL);
        if (device_out) {
            do_install_device_hooks(*device_out);
        }
    } else {
        LOG_WARN("CreateDeviceEx failed (0x%08X)", hr);
    }
    return hr;
}

/* ------------------------------------------------------------------ */
/* Present hooks                                                      */
/* ------------------------------------------------------------------ */

static HRESULT STDMETHODCALLTYPE present_hook(
    void *self, const RECT *src, const RECT *dst, HWND window, const RGNDATA *dirty) {
    InterlockedIncrement64(&g_ev_present);
    if (self == g_paced_device && g_pace_point == PACE_DEVICE) {
        pace_frame();
    }
    return ((present_t)g_real_present)(self, src, dst, window, dirty);
}

static HRESULT STDMETHODCALLTYPE presentex_hook(
    void *self, const RECT *src, const RECT *dst, HWND window,
    const RGNDATA *dirty, DWORD flags) {
    InterlockedIncrement64(&g_ev_present);
    if (self == g_paced_device && g_pace_point == PACE_DEVICE) {
        pace_frame();
    }
    return ((present_ex_t)g_real_presentex)(self, src, dst, window, dirty, flags);
}

/* The first swap chain instance that presents is treated as the primary one:
   the game presents two swap chains per frame (main screen + subscreen) and both
   share this vtable, so pacing every present would hold the loop back twice per
   frame and run the game at half speed. */
static void *g_primary_swapchain;

static HRESULT STDMETHODCALLTYPE swapchain_present_hook(
    void *self, const RECT *src, const RECT *dst, HWND window,
    const RGNDATA *dirty, DWORD flags) {
    InterlockedIncrement64(&g_ev_swapchain);
    if (!g_primary_swapchain) {
        if (InterlockedCompareExchangePointer(&g_primary_swapchain, self, NULL) == NULL) {
            LOG_INFO("primary swap chain instance: %p (further instances are ignored "
                     "for pacing)", self);
        }
    }
    if (self == g_primary_swapchain && g_pace_point == PACE_SWAPCHAIN) {
        pace_frame();
    }
    return ((swapchain_present_t)g_real_sc_present)(self, src, dst, window, dirty, flags);
}

static void do_install_device_hooks(void *device) {
    if (!device) {
        return;
    }
    if (g_paced_device && g_paced_device != device) {
        LOG_INFO("additional device %p created (primary is %p), not paced",
                 device, g_paced_device);
        return;
    }
    if (!g_paced_device) {
        g_paced_device = device;
    }
    if (!g_pacing_enabled) {
        return;
    }
    if (g_cfg.pace_at != PACE_AUTO) {
        InterlockedExchange(&g_pace_point, g_cfg.pace_at);
    }

    /* 1) the implicit swap chain (index 0): this is where the real game
       presents from, and it is the most accurate place to hold the loop back */
    if (g_cfg.pace_at == PACE_AUTO || g_cfg.pace_at == PACE_SWAPCHAIN) {
        void *swapchain = NULL;
        get_swapchain_t get_swapchain =
            (get_swapchain_t)((*(void ***)device)[VTBL_DEVICE_GETSWAPCHAIN]);
        if (get_swapchain &&
            SUCCEEDED(get_swapchain(device, 0, &swapchain)) && swapchain) {
            g_real_sc_present = vtable_patch(swapchain, VTBL_SWAPCHAIN_PRESENT,
                                             (void *)swapchain_present_hook);
            if (g_real_sc_present) {
                LOG_INFO("device %p: implicit swap chain %p Present hooked (real %p)",
                         device, swapchain, g_real_sc_present);
            } else {
                LOG_WARN("device %p: could not hook the swap chain Present", device);
            }
            /* drop our reference; the patch lives in the shared vtable */
            ((release_t)((*(void ***)swapchain)[VTBL_RELEASE]))(swapchain);
        } else {
            LOG_INFO("device %p: GetSwapChain(0) failed, no swap chain hook", device);
        }
    }

    /* 2) IDirect3DDevice9(Ex)::Present / PresentEx */
    if (g_cfg.hook_present) {
        g_real_present = vtable_patch(device, VTBL_DEVICE_PRESENT, (void *)present_hook);
        if (g_cfg.hook_presentex) {
            g_real_presentex = vtable_patch(device, VTBL_DEVICE_PRESENTEX, (void *)presentex_hook);
        }
        if (g_real_present) {
            InterlockedExchange(&g_device_hooked, 1);
            LOG_INFO("device %p: Present hooked (real %p), PresentEx %s",
                     device, g_real_present, g_cfg.hook_presentex ? "hooked" : "not hooked");
        } else {
            LOG_WARN("could not hook Present on device %p", device);
        }
    }
}

/* ------------------------------------------------------------------ */
/* IDirect3D9(Ex) capture                                             */
/* ------------------------------------------------------------------ */

static void *d3d_create_hook(void *d3d) {
    if (d3d) {
        void *o1 = vtable_patch(d3d, VTBL_IDIRECT3D9_CREATEDEVICE, (void *)createdevice_hook);
        void *o2 = vtable_patch(d3d, VTBL_IDIRECT3D9_CREATEDEVICEEX, (void *)createdeviceex_hook);
        if (o1) {
            g_real_createdevice = o1;
        }
        if (o2) {
            g_real_createdeviceex = o2;
        }
        LOG_INFO("IDirect3D9 object %p hooked (CreateDevice=%p CreateDeviceEx=%p)",
                 d3d, g_real_createdevice, g_real_createdeviceex);
    }
    return d3d;
}

static HRESULT WINAPI d3dcreate9ex_hook(UINT sdk_version, void **out) {
    HRESULT hr = ((d3dcreate9ex_t)g_real_d3dcreate9ex)(sdk_version, out);
    if (SUCCEEDED(hr) && out && *out) {
        *out = d3d_create_hook(*out);
    }
    return hr;
}

static void *WINAPI d3dcreate9_hook(UINT sdk_version) {
    void *d3d = ((d3dcreate9_t)g_real_d3dcreate9)(sdk_version);
    return d3d_create_hook(d3d);
}

/* ------------------------------------------------------------------ */
/* EnumDisplaySettings hooks                                          */
/* ------------------------------------------------------------------ */

static void spoof_devmode_a(DEVMODEA *dm, DWORD mode_num) {
    if (!dm || !g_spoof_on || g_measure_only) {
        return;
    }
    if (mode_num == ENUM_CURRENT_SETTINGS || g_cfg.spoof_all_modes) {
        if (dm->dmDisplayFrequency != (DWORD)g_target_fps) {
            LOG_MISC("EnumDisplaySettingsA mode=%lu %lux%lu %lubpp %luHz -> %ldHz",
                     (unsigned long)mode_num,
                     (unsigned long)dm->dmPelsWidth, (unsigned long)dm->dmPelsHeight,
                     (unsigned long)dm->dmBitsPerPel, (unsigned long)dm->dmDisplayFrequency,
                     (long)g_target_fps);
            dm->dmDisplayFrequency = (DWORD)g_target_fps;
        }
    }
}

static void spoof_devmode_w(DEVMODEW *dm, DWORD mode_num) {
    if (!dm || !g_spoof_on || g_measure_only) {
        return;
    }
    if (mode_num == ENUM_CURRENT_SETTINGS || g_cfg.spoof_all_modes) {
        if (dm->dmDisplayFrequency != (DWORD)g_target_fps) {
            LOG_MISC("EnumDisplaySettingsW mode=%lu %lux%lu %lubpp %luHz -> %ldHz",
                     (unsigned long)mode_num,
                     (unsigned long)dm->dmPelsWidth, (unsigned long)dm->dmPelsHeight,
                     (unsigned long)dm->dmBitsPerPel, (unsigned long)dm->dmDisplayFrequency,
                     (long)g_target_fps);
            dm->dmDisplayFrequency = (DWORD)g_target_fps;
        }
    }
}

static BOOL WINAPI eds_a_hook(LPCSTR device, DWORD mode, DEVMODEA *dm) {
    BOOL r = ((eds_a_t)g_real_edsa)(device, mode, dm);
    if (r) {
        spoof_devmode_a(dm, mode);
    }
    return r;
}

static BOOL WINAPI eds_w_hook(LPCWSTR device, DWORD mode, DEVMODEW *dm) {
    BOOL r = ((eds_w_t)g_real_edsw)(device, mode, dm);
    if (r) {
        spoof_devmode_w(dm, mode);
    }
    return r;
}

static BOOL WINAPI eds_exa_hook(LPCSTR device, DWORD mode, DEVMODEA *dm, DWORD flags) {
    BOOL r = ((eds_exa_t)g_real_edsexa)(device, mode, dm, flags);
    if (r) {
        spoof_devmode_a(dm, mode);
    }
    return r;
}

static BOOL WINAPI eds_exw_hook(LPCWSTR device, DWORD mode, DEVMODEW *dm, DWORD flags) {
    BOOL r = ((eds_exw_t)g_real_edsexw)(device, mode, dm, flags);
    if (r) {
        spoof_devmode_w(dm, mode);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* spice2x d3d9 callback: telemetry and draw based pacing fallback    */
/* ------------------------------------------------------------------ */

/* Frame-to-frame interval statistics, measured at whatever point is really
   pacing the loop.  The spice draw callback is the natural place, but older
   spice builds have no register_d3d9, in which case the swap chain (or the
   device present) is the pace point and the numbers have to come from there -
   otherwise a report from such a machine has no jitter data at all. */
static void record_frame_gap(void) {
    LARGE_INTEGER now;
    LONGLONG prev;
    LONGLONG gap;

    QueryPerformanceCounter(&now);
    prev = (LONGLONG)InterlockedExchange64(&g_ev_last_qpc, now.QuadPart);
    if (prev <= 0) {
        return;
    }
    gap = now.QuadPart - prev;
    if (gap <= 0) {
        return;
    }
    /* ignore calls that land inside the same frame (the pace point can fire more
       than once while the loop is still deciding which instance is the primary
       one); a real game frame cannot be shorter than a quarter of the period */
    if (g_period_ticks > 1.0 && (double)gap < g_period_ticks / 4.0) {
        return;
    }
    {
        LONGLONG mn = (LONGLONG)InterlockedCompareExchange64(&g_ev_min_gap, gap, 0);
        LONGLONG mx;
        if (mn == 0 || gap < mn) {
            InterlockedExchange64(&g_ev_min_gap, gap);
        }
        mx = (LONGLONG)InterlockedCompareExchange64(&g_ev_max_gap, gap, 0);
        if (mx == 0 || gap > mx) {
            InterlockedExchange64(&g_ev_max_gap, gap);
        }
    }
}

static void __cdecl d3d9_callback(
    SPICE_SDK_D3D9_EVENT event, const SPICE_SDK_D3D9_FRAME *frame, void *userdata) {
    (void)frame;
    (void)userdata;
    if (event == SPICE_SDK_D3D9_READY) {
        InterlockedExchange(&g_draw_seen, 1);
        return;
    }
    if (event != SPICE_SDK_D3D9_DRAW) {
        return;
    }
    record_frame_gap();
    InterlockedIncrement64(&g_ev_draw);
    /* The cabinet type is only visible in the rom the game reads while booting.
       It is normally settled in the CreateDevice hook (earlier and before the
       monitor check), this is just a safety net for setups where we never see a
       device creation.  Running it from the game's own thread means we never
       touch its filesystem concurrently. */
    if (InterlockedCompareExchange64(&g_ev_draw, 0, 0) == 1) {
        tdj_recheck_from_rom();
    }
    if (g_pace_point == PACE_DRAW) {
        pace_frame();
    }
}

/* ------------------------------------------------------------------ */
/* legacy millisecond clocks -> QueryPerformanceCounter               */
/* ------------------------------------------------------------------ */

/* bm2dx.dll imports timeGetTime/GetTickCount(64) from winmm/kernel32: those are
   the 1ms-resolution clocks the game was written against.
 *
 * The replacement must be *transparent*: same unit, same 32 bit wrap-around and
 * the same epoch (milliseconds since the system started), just with the
 * resolution of the high precision timer instead of the 1ms/15.6ms tick.  So the
 * real clock is read once, before the imports are redirected, and every value
 * after that continues from exactly there.  (Starting the counter at zero when
 * the module loads would hand the game a clock that jumps backwards, which is a
 * real behaviour change and could disturb anything that measures a duration.) */
static ULONGLONG g_ms_base;              /* real uptime in ms when we took over */
static volatile LONG g_ms_base_ready;

static void ms_clock_init(void) {
    LARGE_INTEGER now;

    /* Resolve the genuine clocks directly instead of using the saved "original"
       pointers: those are only filled in by the IAT hook below, which runs after
       this call.  Asking the system DLLs is independent of the game's (not yet
       patched) import table anyway. */
    {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        HMODULE wmm = GetModuleHandleA("winmm.dll");
        ULONGLONG (WINAPI *real64)(void) = NULL;
        DWORD (WINAPI *real32)(void) = NULL;

        if (k32) {
            real64 = (ULONGLONG (WINAPI *)(void))(void *)GetProcAddress(k32, "GetTickCount64");
            real32 = (DWORD (WINAPI *)(void))(void *)GetProcAddress(k32, "GetTickCount");
        }
        if (!real32 && wmm) {
            real32 = (DWORD (WINAPI *)(void))(void *)GetProcAddress(wmm, "timeGetTime");
        }
        if (real64) {
            g_ms_base = real64();
        } else if (real32) {
            g_ms_base = (ULONGLONG)real32();
        }
    }
    QueryPerformanceCounter(&now);
    g_qpc_epoch = now;
    InterlockedExchange(&g_ms_base_ready, 1);
    LOG_INFO("millisecond clocks continue the system uptime "
             "(take over at %llu ms, same epoch and unit as the real ones)",
             (unsigned long long)g_ms_base);
}

static ULONGLONG qpc_uptime_ms(void) {
    LARGE_INTEGER now;
    LONGLONG ticks;

    if (!InterlockedCompareExchange(&g_ms_base_ready, 0, 0)) {
        return g_ms_base;               /* not armed yet: report the take over time */
    }
    QueryPerformanceCounter(&now);
    ticks = now.QuadPart - g_qpc_epoch.QuadPart;
    if (ticks < 0) {
        ticks = 0;
    }
    return g_ms_base + (ULONGLONG)((ticks * 1000) / g_qpf.QuadPart);
}

static DWORD WINAPI timegettime_hook(void) {
    return (DWORD)qpc_uptime_ms();       /* deliberate 32 bit wrap, like the original */
}

static DWORD WINAPI gettickcount_hook(void) {
    return (DWORD)qpc_uptime_ms();
}

static ULONGLONG WINAPI gettickcount64_hook(void) {
    return qpc_uptime_ms();
}

/* ------------------------------------------------------------------ */
/* worker thread                                                      */
/* ------------------------------------------------------------------ */

static int key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static const char *pace_point_name(void) {
    switch (InterlockedCompareExchange(&g_pace_point, 0, 0)) {
        case PACE_SWAPCHAIN: return "swapchain";
        case PACE_DEVICE:    return "present";
        case PACE_DRAW:      return "draw";
        default:             return "auto/undecided";
    }
}

/* higher value = preferred pacing point (the order depends on the variant) */
static void log_status(void) {
    LOG_INFO("STATUS target=%ld fps | paced=%lld late=%lld skipped=%lld | swapchain=%lld "
             "present=%lld draw=%lld | pacePoint=%s | refreshSpoof=%d | deviceHooked=%ld",
             (long)g_target_fps,
             (long long)g_ev_paced, (long long)g_ev_late, (long long)g_ev_skipped,
             (long long)g_ev_swapchain, (long long)g_ev_present, (long long)g_ev_draw,
             pace_point_name(),
             InterlockedCompareExchange(&g_spoof_on, 0, 0), (long)g_device_hooked);
}

/* Ctrl+Shift+F9  cycle target rate 120 <-> 60
   Ctrl+Shift+F10 toggle the monitor refresh spoof
   Ctrl+Shift+F11 print a status line into the spice log */
static void handle_hotkeys(void) {
    static int prev9, prev10, prev11;
    int combo = key_down(VK_CONTROL) && key_down(VK_SHIFT);
    int k9 = combo && key_down(VK_F9);
    int k10 = combo && key_down(VK_F10);
    int k11 = combo && key_down(VK_F11);

    if (k9 && !prev9) {
        int next;
        if (g_target_fps == 120) {
            next = 60;
        } else if (g_target_fps == 60) {
            next = 120;
        } else {
            next = (g_target_fps == 30) ? 120 : 30;
        }
        set_target_fps(next, "hotkey Ctrl+Shift+F9");
    }
    if (k10 && !prev10) {
        LONG v = InterlockedCompareExchange(&g_spoof_on, 0, 0) ? 0 : 1;
        InterlockedExchange(&g_spoof_on, v);
        LOG_INFO("hotkey: display refresh spoof %s", v ? "ON" : "OFF");
    }
    if (k11 && !prev11) {
        log_status();
    }
    prev9 = k9;
    prev10 = k10;
    prev11 = k11;
}

static int g_desktop_refresh;
static int g_off_target_ticks;
static volatile LONG g_offtarget_logged;

/* A small, self contained status file next to the DLL.  Players can send this
   single file back instead of digging through spice's (large) log. */
static char g_report_path[MAX_PATH];

static void report_append(HANDLE f, const char *fmt, ...) {
    char line[512];
    DWORD written = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line) - 3, fmt, ap);
    va_end(ap);
    strcat(line, "\r\n");
    WriteFile(f, line, (DWORD)strlen(line), &written, NULL);
}

static void write_report(double fps_draw, double fps_swapchain, double fps_present,
                         long long paced_per_s, long long late, long long skipped,
                         double gap_min_ms, double gap_max_ms) {
    char line[LOG_RING_LEN + 8];
    HANDLE f;
    DWORD written = 0;
    int i;
    LONG ring;
    SYSTEMTIME st;

    if (g_report_path[0] == '\0' || g_cfg.log_fps == 0) {
        return;
    }
    f = CreateFileA(g_report_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }

    GetLocalTime(&st);
    ring = InterlockedCompareExchange(&g_log_ring_next, 0, 0);

    report_append(f, "iidxfps %s - beatmania IIDX frame timing report", IIDXFPS_VERSION);
    report_append(f, "generated: %04d-%02d-%02d %02d:%02d:%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    report_append(f, "config   : Enabled=%d FPS=%s target=%ld PaceAt=%s ForceImmediatePresent=%d",
                  g_cfg.enabled, g_cfg.target_fps ? "fixed" : "auto", (long)g_target_fps,
                  pace_point_name(), g_cfg.force_immediate);
    report_append(f, "           SpoofDisplayRefresh=%d SpoofAllModes=%d HighPrecisionTime=%d",
                  g_cfg.spoof_refresh, g_cfg.spoof_all_modes, g_cfg.high_precision_time);
    report_append(f, "           MeasureOnly=%d SkipWhenRefreshMatches=%d ForcePacing=%d",
                  g_cfg.measure_only, g_cfg.skip_when_match, g_cfg.force_pacing);
    report_append(f, "           HookPresent=%d HookPresentEx=%d register_d3d9=%s",
                  g_cfg.hook_present, g_cfg.hook_presentex,
                  g_spice.register_d3d9 ? "yes" : "no");
    report_append(f, "desktop  : %dHz   game is told %ldHz",
                  g_desktop_refresh, (long)(g_spoof_on ? g_target_fps : g_desktop_refresh));
    if (g_refresh_fit[0]) {
        report_append(f, "refresh  : %s", g_refresh_fit);
    }
    if (g_refresh_advice[0]) {
        report_append(f, "advice   : %s", g_refresh_advice);
    }
    if (g_cfg.target_fps == 0) {
        report_append(f, "cabinet  : auto -> cmdline=%s spicecfg=%s romfiles=%s game rom=\"%s\"",
                      command_line_says_tdj() ? "TDJ" : "no",
                      spicecfg_config_says_tdj() ? "TDJ" : "no",
                      (file_equals("C:\\000rom.txt", "TDJ-JA") ||
                       file_equals("D:\\001rom.txt", "TDJ")) ? "TDJ" : "no",
                      g_rom_content[0] ? g_rom_content : "(not readable)");
    } else {
        report_append(f, "cabinet  : fixed at %d fps in " INI_NAME, g_cfg.target_fps);
    }
    report_append(f, "pacing   : %s   pacePoint=%s   deviceHooked=%ld",
                  g_measure_only ? "measure only"
                                 : (g_pacing_enabled ? "ENABLED (QPC schedule, vsync off)"
                                                     : "hands off (refresh matches)"),
                  pace_point_name(), (long)g_device_hooked);
    if (g_game_module && g_audit_count >= 0) {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_game_module;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)g_game_module + dos->e_lfanew);
            report_append(f, "game dll : bm2dx base=%p image=%lu bytes build=0x%08lX",
                          g_game_module,
                          (unsigned long)nt->OptionalHeader.SizeOfImage,
                          (unsigned long)nt->FileHeader.TimeDateStamp);
        }
    }
    report_append(f, "footprint: game code bytes modified by this module = NONE");
    report_append(f, "           (only import slots, shared COM vtable slots and present "
                     "parameters; judgement and note logic are never touched)");
    {
        int i;
        for (i = 0; i < g_audit_count; i++) {
            report_append(f, "  [%02d] %-46s slot=%p %p -> %p", i, g_audit[i].what,
                          g_audit[i].slot, g_audit[i].from, g_audit[i].to);
        }
    }
    report_append(f, "frames/s : draw=%.2f swapchain=%.2f present=%.2f paced=%.0f "
                     "late=%lld skipped=%lld",
                  fps_draw, fps_swapchain, fps_present, (double)paced_per_s,
                  (long long)late, (long long)skipped);
    report_append(f, "frame gap: %.3f .. %.3f ms", gap_min_ms, gap_max_ms);
    if (g_halfspeed_logged) {
        report_append(f, "WARNING  : pacing ran more than once per frame (half speed) - "
                         "try HookPresentEx=0");
    }
    if (g_offtarget_logged) {
        report_append(f, "WARNING  : hands off but the game does not run at the target rate - "
                         "set ForcePacing=1");
    }
    report_append(f, "--- last messages (oldest first), ring next=%ld size=%d ---", (long)ring, LOG_RING_SIZE);
    for (i = 0; i < LOG_RING_SIZE; i++) {
        LONG slot = (ring + i) % LOG_RING_SIZE;
        const char *msg = g_log_ring[slot];
        if (msg[0] == '\0') {
            continue;
        }
        _snprintf(line, sizeof(line) - 1, "%s", msg);
        line[sizeof(line) - 1] = '\0';
        report_append(f, "[%02ld] %s", slot, line);
    }
    (void)written;
    CloseHandle(f);
}


/* ------------------------------------------------------------------ */
/* refresh rate compatibility                                         */
/* ------------------------------------------------------------------ */
/*
 * IIDX counts frames, and no external tool can invent frames the game never
 * rendered.  So if the display scans out at a rate that is neither an integer
 * multiple nor an integer divisor of the target, each game frame is shown for a
 * whole number of refreshes only *on average*: 144Hz with a 120 fps target is
 * 1.2 refreshes per frame, i.e. a repeating 1,1,1,1,2 pattern whose extra
 * refresh lands every 5th frame (a 24Hz beat).  Players describe that as the
 * notes looking like jelly.
 *
 * The module cannot fix the arithmetic, but it can say exactly what is wrong
 * and, when the panel offers a rate that does fit, name it.
 */
#define REFRESH_TOLERANCE_PCT 2      /* 59.94 and 119.88 style modes must pass */

static int refresh_is_compatible(int refresh, int target) {
    int n;

    if (refresh <= 0 || target <= 0) {
        return 1;                    /* nothing to judge yet */
    }
    for (n = 1; n <= 8; n++) {
        int diff;
        diff = refresh - n * target;             /* refresh = n x target */
        if (diff < 0) {
            diff = -diff;
        }
        if (diff * 100 <= target * REFRESH_TOLERANCE_PCT) {
            return 1;
        }
        diff = target - n * refresh;             /* target = n x refresh */
        if (diff < 0) {
            diff = -diff;
        }
        if (diff * 100 <= refresh * REFRESH_TOLERANCE_PCT) {
            return 1;
        }
    }
    return 0;
}

/* score: 0 = 1:1, 1 = refresh is a multiple of the target (each game frame gets
   n refreshes), 2 = the target is a multiple of the refresh (each refresh shows
   n game frames).  Lower is better; -1 means not compatible. */
static int refresh_fit_score(int refresh, int target) {
    int n;

    if (refresh <= 0 || target <= 0) {
        return -1;
    }
    for (n = 1; n <= 8; n++) {
        int diff = refresh - n * target;
        if (diff < 0) {
            diff = -diff;
        }
        if (diff * 100 <= target * REFRESH_TOLERANCE_PCT) {
            return (n == 1) ? 0 : 1;
        }
    }
    for (n = 2; n <= 8; n++) {
        int diff = target - n * refresh;
        if (diff < 0) {
            diff = -diff;
        }
        if (diff * 100 <= refresh * REFRESH_TOLERANCE_PCT) {
            return 2;
        }
    }
    return -1;
}

/* Ask user32 directly (our hooks only touch the game's import table) for the
   refresh rates this panel really offers at the resolution it is running at,
   and pick the one that fits the target best. */
static int find_compatible_refresh(int target, int *out_hz) {
    DEVMODEA dm;
    DWORD mode = 0;
    int seen[64];
    int seen_count = 0;
    int best = 0;
    int best_score = 99;
    DWORD cur_w = 0, cur_h = 0;

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        cur_w = dm.dmPelsWidth;
        cur_h = dm.dmPelsHeight;
    }
    for (mode = 0; mode < 4096; mode++) {
        int hz, i, dup = 0;

        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(NULL, mode, &dm)) {
            break;
        }
        if (cur_w && cur_h && (dm.dmPelsWidth != cur_w || dm.dmPelsHeight != cur_h)) {
            continue;                                  /* only this resolution */
        }
        hz = (int)dm.dmDisplayFrequency;
        if (hz <= 0) {
            continue;
        }
        for (i = 0; i < seen_count; i++) {
            if (seen[i] == hz) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (seen_count < (int)(sizeof(seen) / sizeof(seen[0]))) {
            seen[seen_count++] = hz;
        }

        {
            int score = refresh_fit_score(hz, target);
            if (score >= 0 && (score < best_score ||
                               (score == best_score && hz > best))) {
                best_score = score;
                best = hz;
            }
        }
    }
    if (best > 0) {
        *out_hz = best;
        return 1;
    }
    return 0;
}

static void analyse_refresh_fit(void) {
    int refresh = g_desktop_refresh;
    int target = (int)g_target_fps;
    int n;

    g_refresh_fit[0] = '\0';
    g_refresh_advice[0] = '\0';
    if (refresh <= 0 || target <= 0) {
        return;
    }
    /* express the ratio as "x.xx refreshes per game frame" or, when the target
       is the faster one, as "x.xx game frames per refresh" */
    if (refresh >= target) {
        n = (int)(((double)refresh / (double)target) * 100.0 + 0.5);
        _snprintf(g_refresh_fit, sizeof(g_refresh_fit) - 1,
                  "%dHz / %d fps = %d.%02d refreshes per game frame",
                  refresh, target, n / 100, n % 100);
    } else {
        n = (int)(((double)target / (double)refresh) * 100.0 + 0.5);
        _snprintf(g_refresh_fit, sizeof(g_refresh_fit) - 1,
                  "%dHz / %d fps = %d.%02d game frames per refresh",
                  refresh, target, n / 100, n % 100);
    }
    g_refresh_fit[sizeof(g_refresh_fit) - 1] = '\0';

    if (refresh_is_compatible(refresh, target)) {
        return;
    }
    {
        int alt = 0;
        int beat = (refresh - target < 0 ? target - refresh : refresh - target);
        if (find_compatible_refresh(target, &alt)) {
            int score = refresh_fit_score(alt, target);
            if (score == 0) {
                _snprintf(g_refresh_advice, sizeof(g_refresh_advice) - 1,
                          "not an integer ratio: note motion will judder (a %dHz beat). "
                          "Set the display to %dHz: that is exactly %d fps, 1:1",
                          beat, alt, target);
            } else if (score == 1) {
                int n = (alt + target / 2) / target;
                _snprintf(g_refresh_advice, sizeof(g_refresh_advice) - 1,
                          "not an integer ratio: note motion will judder (a %dHz beat). "
                          "Set the display to %dHz: %d refreshes per game frame, perfectly even",
                          beat, alt, n);
            } else {
                int n = (target + alt / 2) / alt;
                _snprintf(g_refresh_advice, sizeof(g_refresh_advice) - 1,
                          "not an integer ratio: note motion will judder (a %dHz beat). "
                          "Set the display to %dHz: %d game frames per refresh, even motion "
                          "at a lower refresh rate",
                          beat, alt, n);
            }
        } else {
            _snprintf(g_refresh_advice, sizeof(g_refresh_advice) - 1,
                      "not an integer ratio: note motion will judder (a %dHz beat) and this "
                      "monitor offers no compatible rate at this resolution; use a 120Hz or "
                      "240Hz display, or a resolution that can run at one of those",
                      (refresh - target < 0 ? target - refresh : refresh - target));
        }
        g_refresh_advice[sizeof(g_refresh_advice) - 1] = '\0';
        LOG_WARN("%s", g_refresh_fit);
        LOG_WARN("%s", g_refresh_advice);
    }
}

static void log_display_state(void) {
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    g_desktop_refresh = 0;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        g_desktop_refresh = (int)dm.dmDisplayFrequency;
        LOG_INFO("desktop mode: %lux%lu %lubpp %luHz (game is told %ldHz)",
                 (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
                 (unsigned long)dm.dmBitsPerPel, (unsigned long)dm.dmDisplayFrequency,
                 (long)(g_cfg.spoof_refresh ? g_target_fps : (LONG)dm.dmDisplayFrequency));
    }
}

/* Decide whether to touch anything at all.
 *
 * If the desktop already runs at (very close to) the rate the game needs, the
 * game's own vsync is already keeping it at the right speed and turning vsync
 * off would only add tearing.  In that case stay completely hands off; the user
 * can force exact timing with ForcePacing=1.
 */
static void decide_pacing(void) {
    int target = (int)g_target_fps;
    int diff;

    g_pacing_enabled = g_cfg.enabled && !g_measure_only;
    if (!g_pacing_enabled) {
        return;
    }
    if (g_cfg.force_pacing || !g_cfg.skip_when_match || g_desktop_refresh <= 0) {
        return;
    }
    diff = g_desktop_refresh - target;
    if (diff < 0) {
        diff = -diff;
    }
    if (diff * 100 <= target * 3) {
        g_pacing_enabled = 0;
        LOG_INFO("desktop refresh %dHz already matches the target %d fps: leaving vsync and "
                 "the frame rate alone so the picture stays tear free "
                 "(set ForcePacing=1 in " INI_NAME " to force exact QPC timing anyway)",
                 g_desktop_refresh, target);
    }
}

static void install_game_hooks(void) {
    if (!g_cfg.enabled || g_measure_only) {
        LOG_INFO("MeasureOnly / disabled: no hooks installed, only measuring");
        return;
    }

    if (g_pacing_enabled) {
        int n = 0;
        n += iat_hook(g_game_module, "d3d9.dll", "Direct3DCreate9Ex",
                      (void *)d3dcreate9ex_hook, &g_real_d3dcreate9ex);
        n += iat_hook(g_game_module, "d3d9.dll", "Direct3DCreate9",
                      (void *)d3dcreate9_hook, &g_real_d3dcreate9);
        LOG_INFO("d3d9 entry points hooked in bm2dx.dll: %d", n);
        if (n == 0) {
            LOG_WARN("could not hook Direct3DCreate9Ex in bm2dx.dll "
                     "(device creation will not be intercepted)");
        }
    }

    if (g_cfg.spoof_refresh) {
        int n = 0;
        n += iat_hook(g_game_module, "user32.dll", "EnumDisplaySettingsA",
                      (void *)eds_a_hook, &g_real_edsa);
        n += iat_hook(g_game_module, "user32.dll", "EnumDisplaySettingsW",
                      (void *)eds_w_hook, &g_real_edsw);
        n += iat_hook(g_game_module, "user32.dll", "EnumDisplaySettingsExA",
                      (void *)eds_exa_hook, &g_real_edsexa);
        n += iat_hook(g_game_module, "user32.dll", "EnumDisplaySettingsExW",
                      (void *)eds_exw_hook, &g_real_edsexw);
        LOG_INFO("EnumDisplaySettings imports hooked: %d (reporting %ldHz)",
                 n, (long)g_target_fps);
        if (n == 0) {
            LOG_WARN("bm2dx.dll does not import EnumDisplaySettings*, refresh spoof is a no-op");
        }
    }

    if (g_cfg.high_precision_time) {
        int n = 0;
        /* read the real clocks first, then redirect the imports a moment later:
           the replacement continues from the value the game would have seen */
        ms_clock_init();
        n += iat_hook(g_game_module, "winmm.dll", "timeGetTime",
                      (void *)timegettime_hook, &g_real_timegettime);
        n += iat_hook(g_game_module, "kernel32.dll", "GetTickCount",
                      (void *)gettickcount_hook, &g_real_gettickcount);
        n += iat_hook(g_game_module, "kernel32.dll", "GetTickCount64",
                      (void *)gettickcount64_hook, &g_real_gettickcount64);
        LOG_INFO("millisecond clocks replaced by QPC derived values: %d import(s)", n);
    }
}

static unsigned __stdcall worker_thread(void *arg) {
    LARGE_INTEGER last;
    LONG64 last_present = 0, last_draw = 0, last_paced = 0;
    int ticks = 0;
    int i;

    (void)arg;

    for (i = 0; i < 120000; i++) {
        g_game_module = GetModuleHandleA("bm2dx.dll");
        if (!g_game_module) {
            g_game_module = GetModuleHandleA("bm2dx_omni.dll");
        }
        if (g_game_module) {
            break;
        }
        Sleep(1);
    }
    if (!g_game_module) {
        LOG_WARN("bm2dx.dll was not loaded within 120s, giving up");
        return 0;
    }
    LOG_INFO("game module found at %p", g_game_module);

    resolve_target_fps();
    log_display_state();
    decide_pacing();
    analyse_refresh_fit();

    install_game_hooks();

    LOG_INFO("pacing: %s, target %ld fps, pacing point = %s",
             g_pacing_enabled ? "ENABLED" : "disabled", (long)g_target_fps, pace_point_name());

    QueryPerformanceCounter(&last);
    {
        double accum = 1.0;
        LONG64 last_swapchain = 0;
        LONG64 sel_sc = 0, sel_dv = 0, sel_dr = 0;
        for (;;) {
            LARGE_INTEGER now;
            double dt;
            LONG64 swapchain, present, draw, paced, late, skipped;

            Sleep(20);

            QueryPerformanceCounter(&now);
            dt = (double)(now.QuadPart - last.QuadPart) / (double)g_qpf.QuadPart;
            last = now;
            accum += dt;

            handle_hotkeys();

            /* Pick the pacing point that is really being called, sampled every
               20ms.  v1 paces before the present, so the spice draw callback
               (exactly once per game frame) is the best point.  v2 paces after
               the present, so the swap chain / device Present wins and the draw
               callback is only a fallback.  Switching to a *lower* priority
               point only happens when the current one went completely silent,
               which stops the selection from flapping. */
            if (g_pacing_enabled && g_cfg.pace_at == PACE_AUTO) {
                LONG64 cs = (LONG64)InterlockedCompareExchange64(&g_ev_swapchain, 0, 0);
                LONG64 cd = (LONG64)InterlockedCompareExchange64(&g_ev_present, 0, 0);
                LONG64 cr = (LONG64)InterlockedCompareExchange64(&g_ev_draw, 0, 0);
                LONG64 ds = cs - sel_sc, dd = cd - sel_dv, dr = cr - sel_dr;
                int cur = InterlockedCompareExchange(&g_pace_point, 0, 0);
                int cur_events;
                int sel = 0;

                if (dr > 0) {
                    sel = PACE_DRAW;
                } else if (ds > 0) {
                    sel = PACE_SWAPCHAIN;
                } else if (dd > 0) {
                    sel = PACE_DEVICE;
                }

                cur_events = (cur == PACE_DRAW) ? (int)dr
                           : (cur == PACE_SWAPCHAIN) ? (int)ds
                           : (cur == PACE_DEVICE) ? (int)dd : 0;
                sel_sc = cs;
                sel_dv = cd;
                sel_dr = cr;

                if (sel && sel != cur &&
                    (sel > cur || cur_events == 0)) {
                    InterlockedExchange(&g_pace_point, sel);
                    g_pacer_started = 0;
                    LOG_INFO("pacing point selected: %s", pace_point_name());
                }
            }

            if (accum < 1.0) {
                continue;
            }
            dt = accum;
            accum = 0.0;
            ticks++;

            /* NOTE: the cabinet rom is only ever asked for from the game's own
               thread (the EnumDisplaySettings hook, the CreateDevice hook and the
               first draw callback).  Calling the AVS filesystem from this thread
               hangs on a real (non injected) filesystem access, so never do it
               here. */

            swapchain = (LONG64)InterlockedCompareExchange64(&g_ev_swapchain, 0, 0);
            present = (LONG64)InterlockedCompareExchange64(&g_ev_present, 0, 0);
            draw = (LONG64)InterlockedCompareExchange64(&g_ev_draw, 0, 0);
            paced = (LONG64)InterlockedCompareExchange64(&g_ev_paced, 0, 0);
            late = (LONG64)InterlockedCompareExchange64(&g_ev_late, 0, 0);
            skipped = (LONG64)InterlockedCompareExchange64(&g_ev_skipped, 0, 0);

            /* hands off because the desktop already matches the target: make sure
               the game really does run at the rate the display promises, and tell
               the user how to force exact timing if it does not */
            if (!g_pacing_enabled && !g_measure_only && !g_cfg.force_pacing) {
                double measured = (double)(draw - last_draw) / dt;
                if (draw - last_draw <= 0) {
                    measured = (double)(swapchain - last_swapchain) / dt;
                }
                if (measured > 1.0 && ticks >= 3 &&
                    (measured > (double)g_target_fps * 1.015 ||
                     measured < (double)g_target_fps * 0.985)) {
                    g_off_target_ticks++;
                    if (g_off_target_ticks == 5 &&
                        !InterlockedCompareExchange(&g_offtarget_logged, 1, 0)) {
                        LOG_WARN("the game runs at %.2f fps but the target is %ld fps although "
                                 "the desktop reports %dHz; set ForcePacing=1 in " INI_NAME
                                 " to lock it with the high precision timer",
                                 measured, (long)g_target_fps, g_desktop_refresh);
                    }
                } else {
                    g_off_target_ticks = 0;
                }
            }

            {
                LONG64 mn = (LONG64)InterlockedExchange64(&g_ev_min_gap, 0);
                LONG64 mx = (LONG64)InterlockedExchange64(&g_ev_max_gap, 0);
                double mn_ms = mn > 0 ? (double)mn * 1000.0 / (double)g_qpf.QuadPart : 0.0;
                double mx_ms = mx > 0 ? (double)mx * 1000.0 / (double)g_qpf.QuadPart : 0.0;

                if (g_cfg.log_fps) {
                    LOG_MISC("fps(swapchain)=%.2f fps(present)=%.2f fps(draw)=%.2f pacePoint=%s "
                             "paced=%lld late=%lld skip=%lld gap=%.3f..%.3f ms target=%ld",
                             (double)(swapchain - last_swapchain) / dt,
                             (double)(present - last_present) / dt,
                             (double)(draw - last_draw) / dt,
                             pace_point_name(),
                             (long long)(paced - last_paced),
                             (long long)late, (long long)skipped,
                             mn_ms, mx_ms,
                             (long)g_target_fps);
                }

                write_report((double)(draw - last_draw) / dt,
                             (double)(swapchain - last_swapchain) / dt,
                             (double)(present - last_present) / dt,
                             (long long)(paced - last_paced), late, skipped,
                             mn_ms, mx_ms);
            }

            /* both Present and PresentEx hooked and both being called would
               slow the game to half speed: say so loudly */
            if (InterlockedCompareExchange(&g_pace_point, 0, 0) == PACE_DEVICE &&
                g_cfg.hook_presentex && (paced - last_paced) > (LONG64)g_target_fps * 3 / 2 &&
                !InterlockedCompareExchange(&g_halfspeed_logged, 1, 0)) {
                LOG_WARN("pacing runs %.2f times per frame: the game appears to call both "
                         "Present and PresentEx, set HookPresentEx=0 in " INI_NAME,
                         (double)(paced - last_paced) / dt);
            }

            last_swapchain = swapchain;
            last_present = present;
            last_draw = draw;
            last_paced = paced;

            if (ticks == 5 && g_pacing_enabled && !g_draw_seen && !g_device_hooked &&
                !InterlockedCompareExchange(&g_fallback_logged, 1, 0)) {
                LOG_WARN("no d3d9 device seen after 5s. If the game frame rate is still wrong, "
                         "try PaceAt=draw in " INI_NAME);
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* entry points                                                       */
/* ------------------------------------------------------------------ */

static HANDLE g_thread;

static void __cdecl destroy_callback(void) {
    LOG_INFO("plugin unloading");
    InterlockedExchange(&g_pacing_enabled, 0);
}

SPICE_SDK_ENTRY_POINT
spice_sdk_entry_point(spice_sdk_init_func *init) {
    SPICE_SDK_STATUS_CODE status;
    char ini_path[MAX_PATH];
    int have_ini = 0;

    memset(&g_spice, 0, sizeof(g_spice));
    memset(&g_cfg, 0, sizeof(g_cfg));
    memset(ini_path, 0, sizeof(ini_path));
    g_spice.size = sizeof(g_spice);
    g_cfg.enabled = 1;
    g_cfg.force_immediate = 1;
    g_cfg.spoof_refresh = 1;
    g_cfg.spoof_all_modes = 1;
    g_cfg.log_fps = 1;
    g_cfg.pace_at = 0;

    status = init(0, destroy_callback, &g_spice);
    if (status != SPICE_SDK_STATUS_SUCCESS) {
        return 0;
    }

    InitializeCriticalSection(&g_log_lock);
    g_log_lock_ready = 1;
    InitializeCriticalSection(&g_vtable_lock);

    if (g_spice.get_plugin_directory) {
        wchar_t wide[MAX_PATH];
        uint32_t size = MAX_PATH;
        memset(wide, 0, sizeof(wide));
        if (g_spice.get_plugin_directory((const void *)&spice_sdk_entry_point,
                                         wide, &size) == SPICE_SDK_STATUS_SUCCESS) {
            char narrow[MAX_PATH * 2];
            size_t len;
            memset(narrow, 0, sizeof(narrow));
            WideCharToMultiByte(CP_ACP, 0, wide, -1, narrow, sizeof(narrow) - 1, NULL, NULL);
            len = strlen(narrow);
            while (len > 0 && (narrow[len - 1] == '\\' || narrow[len - 1] == '/')) {
                narrow[--len] = '\0';
            }
            _snprintf(ini_path, sizeof(ini_path) - 1, "%s\\%s", narrow, INI_NAME);
            have_ini = 1;
        }
    }
    if (!have_ini) {
        char exe[MAX_PATH];
        char *slash;
        memset(exe, 0, sizeof(exe));
        GetModuleFileNameA(NULL, exe, sizeof(exe) - 1);
        slash = strrchr(exe, '\\');
        if (slash) {
            *(slash + 1) = '\0';
        }
        _snprintf(ini_path, sizeof(ini_path) - 1, "%s%s", exe, INI_NAME);
    }
    ini_path[sizeof(ini_path) - 1] = '\0';

    /* the report file sits next to the ini */
    {
        char dir[MAX_PATH];
        size_t len;
        memset(dir, 0, sizeof(dir));
        _snprintf(dir, sizeof(dir) - 1, "%s", ini_path);
        len = strlen(dir);
        while (len > 0 && dir[len - 1] != '\\' && dir[len - 1] != '/') {
            dir[--len] = '\0';
        }
        _snprintf(g_report_path, sizeof(g_report_path) - 1, "%s%s", dir, REPORT_NAME);
        g_report_path[sizeof(g_report_path) - 1] = '\0';
    }

    config_init(ini_path);
    LOG_INFO("iidxfps %s plugin loaded, config file: %s", IIDXFPS_VERSION, ini_path);
    LOG_INFO("enabled=%d fps=%d paceAt=%s forceImmediate=%d spoofRefresh=%d spoofAllModes=%d "
             "logFps=%d measureOnly=%d hookPresent=%d hookPresentEx=%d skipWhenMatch=%d "
             "forcePacing=%d",
             g_cfg.enabled, g_cfg.target_fps, pace_point_name(),
             g_cfg.force_immediate, g_cfg.spoof_refresh, g_cfg.spoof_all_modes,
             g_cfg.log_fps, g_cfg.measure_only, g_cfg.hook_present, g_cfg.hook_presentex,
             g_cfg.skip_when_match, g_cfg.force_pacing);

    if (!g_cfg.enabled) {
        LOG_INFO("disabled by config, nothing to do");
        return 1;
    }

    pacer_global_init();
    InterlockedExchange(&g_measure_only, g_cfg.measure_only);
    InterlockedExchange(&g_spoof_on, g_cfg.spoof_refresh);

    if (g_spice.register_d3d9) {
        status = g_spice.register_d3d9(d3d9_callback, NULL);
        if (status == SPICE_SDK_STATUS_SUCCESS) {
            LOG_INFO("registered for d3d9 events");
        } else {
            LOG_WARN("register_d3d9 failed (%d)", (int)status);
        }
    } else {
        LOG_WARN("this spice build has no register_d3d9, using the d3d9 import hooks only");
    }

    g_thread = (HANDLE)_beginthreadex(NULL, 0, worker_thread, NULL, 0, NULL);
    if (!g_thread) {
        LOG_WARN("failed to start the worker thread");
        return 0;
    }

    return 1;
}



















