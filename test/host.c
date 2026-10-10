/*
 * host.exe - loads a fake bm2dx.dll plus iidxfps.dll, hands iidxfps a minimal
 * spice2x SDK implementation and then runs the stub's frame loop.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spicesdk.h"

static SPICE_SDK_V0 g_spice;

static SPICE_SDK_STATUS_CODE __cdecl host_log(
    SPICE_SDK_LOG_LEVEL level, const char *module, const char *message) {
    static const char *names[] = { "M", "I", "W", "F" };
    printf("[%s][%s] %s\n", names[(int)level & 3], module ? module : "?", message ? message : "");
    fflush(stdout);
    return SPICE_SDK_STATUS_SUCCESS;
}

static SPICE_SDK_STATUS_CODE __cdecl host_game_info(SPICE_SDK_GAME_INFO *info) {
    snprintf(info->name, sizeof(info->name), "Beatmania IIDX");
    return SPICE_SDK_STATUS_SUCCESS;
}

static SPICE_SDK_STATUS_CODE __cdecl host_avs_info(SPICE_SDK_AVS_INFO *info) {
    memset(info, 0, sizeof(*info));
    snprintf(info->model, sizeof(info->model), "MDX");
    info->dest = 'J';
    info->spec = 'A';
    info->rev = 'A';
    snprintf(info->ext, sizeof(info->ext), "2026010100");
    return SPICE_SDK_STATUS_SUCCESS;
}

static SPICE_SDK_STATUS_CODE __cdecl host_register_d3d9(
    spice_sdk_d3d9_callback_func *cb, void *userdata) {
    printf("[host] register_d3d9(cb=%p, userdata=%p)\n", (void *)cb, userdata);
    return SPICE_SDK_STATUS_SUCCESS;
}

static SPICE_SDK_STATUS_CODE __cdecl host_init(
    uint32_t version, spice_sdk_destroy_callback_func *destroy, void *sdk_functions) {
    SPICE_SDK_V0 *v0 = (SPICE_SDK_V0 *)sdk_functions;
    (void)version;
    (void)destroy;
    printf("[host] init called, size=%u, destroy=%p\n", v0->size, (void *)destroy);
    if (v0->size < sizeof(SPICE_SDK_V0)) {
        return SPICE_SDK_STATUS_TOO_SMALL;
    }
    v0->log = host_log;
    v0->get_game_info = host_game_info;
    v0->get_avs_info = host_avs_info;
    v0->register_d3d9 = host_register_d3d9;
    return SPICE_SDK_STATUS_SUCCESS;
}

extern USHORT WINAPI RtlCaptureStackBackTrace(ULONG, ULONG, PVOID *, PULONG);

static void print_frame(void *addr, int depth) {
    HMODULE mods[256];
    DWORD needed = 0, i;
    char path[MAX_PATH];
    DWORD64 a = (DWORD64)addr;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        printf("[host]   #%d %p (EnumProcessModules failed %lu)\n", depth, addr, GetLastError());
        return;
    }
    for (i = 0; i < needed / sizeof(HMODULE); i++) {
        DWORD64 base = (DWORD64)mods[i];
        IMAGE_DOS_HEADER *dos;
        IMAGE_NT_HEADERS *nt;
        if (!base) {
            continue;
        }
        dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            continue;
        }
        nt = (IMAGE_NT_HEADERS *)((uint8_t *)base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            continue;
        }
        if (a >= base && a < base + nt->OptionalHeader.SizeOfImage) {
            GetModuleFileNameA(mods[i], path, sizeof(path));
            printf("[host]   #%d %p  %s + 0x%llX\n", depth, addr, path,
                   (unsigned long long)(a - base));
            return;
        }
    }
    printf("[host]   #%d %p  (unknown module, %lu modules)\n", depth, addr,
           (unsigned long)(needed / sizeof(HMODULE)));
}

static LONG CALLBACK veh_handler(EXCEPTION_POINTERS *info) {
    CONTEXT *c = info->ContextRecord;
    uint64_t *sp = (uint64_t *)c->Rsp;
    int i;
    printf("[host] EXCEPTION code=0x%08lX rip=%p rax=%p rcx=%p rdx=%p rsp=%p access=%p type=%llu\n",
           (unsigned long)info->ExceptionRecord->ExceptionCode,
           (void *)c->Rip, (void *)c->Rax, (void *)c->Rcx, (void *)c->Rdx, (void *)c->Rsp,
           (void *)info->ExceptionRecord->ExceptionInformation[1],
           (unsigned long long)info->ExceptionRecord->ExceptionInformation[0]);
    for (i = 0; i < 24; i++) {
        print_frame((void *)sp[i], i);
    }
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char **argv) {
    double fps;
    HMODULE game, plugin;
    spice_sdk_entry_point_func *entry;
    double (*test_run)(int);
    int seconds = 5;
    const char *module_name = "iidxfps.dll";

    if (argc > 1) {
        seconds = atoi(argv[1]);
    }
    if (argc > 2) {
        module_name = argv[2];
    }

    AddVectoredExceptionHandler(1, veh_handler);

    printf("[host] loading stub bm2dx.dll\n");
    game = LoadLibraryA("bm2dx.dll");
    if (!game) {
        printf("[host] failed to load bm2dx.dll: %lu\n", GetLastError());
        return 1;
    }

    printf("[host] loading %s\n", module_name);
    plugin = LoadLibraryA(module_name);
    if (!plugin) {
        printf("[host] failed to load %s: %lu\n", module_name, GetLastError());
        return 1;
    }

    entry = (spice_sdk_entry_point_func *)GetProcAddress(plugin, "spice_sdk_entry_point");
    if (!entry) {
        printf("[host] spice_sdk_entry_point not found\n");
        return 1;
    }
    if (!entry(host_init)) {
        printf("[host] plugin entry point returned 0\n");
        return 1;
    }

    test_run = (double (*)(int))GetProcAddress(game, "test_run");
    if (!test_run) {
        printf("[host] test_run not found in stub\n");
        return 1;
    }

    /* give the plugin worker thread time to install its import hooks */
    Sleep(1500);

    printf("[host] running stub frame loop for %d s\n", seconds);
    fps = test_run(seconds);
    printf("[host] measured loop rate: %.2f fps\n", fps);

    return 0;
}


