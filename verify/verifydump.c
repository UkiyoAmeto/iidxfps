/*
 * verifydump - a *read only* spice2x module used to prove that another module
 * (iidxfps) does not modify the game's executable code.
 *
 * It installs no hooks and patches nothing.  A few seconds after bm2dx.dll is
 * loaded it copies the raw bytes of the module's sections out of the running
 * process into files next to the game, and writes a small text file with the
 * size and sha256 of every section.  Run the game once without the module under
 * test and once with it, then compare the dumps: the .text (and .pdata) image
 * must be byte identical, and any difference in .rdata must be inside the import
 * address table entries that the module under test legitimately redirects.
 *
 * GPL-3.0, same as the rest of this repository.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>

#include "spicesdk.h"

#define SECTION_COUNT_MAX 8
#define SECTION_NAME_MAX  16

typedef struct {
    char     name[SECTION_NAME_MAX];
    uint32_t rva;
    uint32_t size;
    char     sha256[65];
    char     file[MAX_PATH];
} section_info_t;

static SPICE_SDK_V0 g_spice;
static char g_dir[MAX_PATH];

SPICE_SDK_ENTRY_POINT
spice_sdk_entry_point(spice_sdk_init_func *init) {
    SPICE_SDK_STATUS_CODE status;
    wchar_t wide[MAX_PATH];
    uint32_t size = MAX_PATH;

    memset(&g_spice, 0, sizeof(g_spice));
    g_spice.size = sizeof(g_spice);
    status = init(0, NULL, &g_spice);
    if (status != SPICE_SDK_STATUS_SUCCESS) {
        return 0;
    }
    if (g_spice.get_plugin_directory) {
        memset(wide, 0, sizeof(wide));
        if (g_spice.get_plugin_directory((const void *)&spice_sdk_entry_point,
                                         wide, &size) == SPICE_SDK_STATUS_SUCCESS) {
            WideCharToMultiByte(CP_ACP, 0, wide, -1, g_dir, sizeof(g_dir) - 1, NULL, NULL);
        }
    }
    return 1;
}

static void to_hex(const unsigned char *in, size_t len, char *out) {
    static const char *d = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 15];
    }
    out[len * 2] = '\0';
}

static int sha256_of(const unsigned char *data, size_t len, char *hex) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    DWORD obj_size = 0, got = 0, digest_size = 0;
    unsigned char *obj = NULL;
    int ok = 0;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) {
        return 0;
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_size, sizeof(obj_size),
                          &got, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return 0;
    }
    obj = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, obj_size);
    if (obj &&
        BCryptCreateHash(alg, &hash, obj, obj_size, NULL, 0, 0) == 0 &&
        BCryptHashData(hash, (PUCHAR)data, (ULONG)len, 0) == 0 &&
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&digest_size, sizeof(digest_size),
                          &got, 0) == 0 &&
        digest_size == 32 &&
        BCryptFinishHash(hash, digest, 32, 0) == 0) {
        to_hex(digest, 32, hex);
        ok = 1;
    }
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (obj) {
        HeapFree(GetProcessHeap(), 0, obj);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static void dump_one_module(const char *module_name, const char *prefix) {
    HMODULE mod = GetModuleHandleA(module_name);
    section_info_t info[SECTION_COUNT_MAX];
    int count = 0;
    int i;
    FILE *log;
    char log_path[MAX_PATH];
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    int nsec;
    DWORD written = 0;
    int nsec_total;

    if (!mod) {
        return;
    }
    dos = (IMAGE_DOS_HEADER *)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    nt = (IMAGE_NT_HEADERS *)((unsigned char *)mod + dos->e_lfanew);
    nsec_total = nt->FileHeader.NumberOfSections;
    sec = IMAGE_FIRST_SECTION(nt);

    for (i = 0; i < nsec_total && count < SECTION_COUNT_MAX; i++, sec++) {
        char name[SECTION_NAME_MAX];
        uint32_t rva = sec->VirtualAddress;
        uint32_t vsize = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        unsigned char *addr;
        HANDLE f;
        int k;

        memset(name, 0, sizeof(name));
        for (k = 0; k < 8 && sec->Name[k]; k++) {
            name[k] = (char)sec->Name[k];
        }
        if (_stricmp(name, ".text") != 0 && _stricmp(name, ".pdata") != 0 &&
            _stricmp(name, ".rdata") != 0) {
            continue;
        }
        if (vsize == 0 || vsize > 64u * 1024u * 1024u) {
            continue;
        }
        addr = (unsigned char *)mod + rva;

        memset(info[count].name, 0, sizeof(info[count].name));
        for (k = 0; k < (int)sizeof(info[count].name) - 1 && name[k]; k++) {
            info[count].name[k] = name[k];
        }
        info[count].rva = rva;
        info[count].size = vsize;
        _snprintf(info[count].file, sizeof(info[count].file) - 1, "%sverifydump_%s%s.bin",
                  g_dir, prefix, name[1] == 't' ? "text" : (name[1] == 'p' ? "pdata" : "rdata"));
        if (!sha256_of(addr, vsize, info[count].sha256)) {
            continue;
        }
        f = CreateFileA(info[count].file, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            size_t off = 0;
            while (off < vsize) {
                DWORD chunk = (DWORD)((vsize - off) > (1u << 20) ? (1u << 20) : (vsize - off));
                if (!WriteFile(f, addr + off, chunk, &written, NULL) || written != chunk) {
                    break;
                }
                off += chunk;
            }
            CloseHandle(f);
        }
        count++;
    }

    _snprintf(log_path, sizeof(log_path) - 1, "%sverifydump.txt", g_dir);
    log = fopen(log_path, count ? "a" : "a");
    if (!log) {
        return;
    }
    fprintf(log, "--- %s ---\n", module_name);
    fprintf(log, "base=%p image=%lu build=0x%08lX sections=%d\n",
            mod, (unsigned long)nt->OptionalHeader.SizeOfImage,
            (unsigned long)nt->FileHeader.TimeDateStamp, nsec_total);
    for (i = 0; i < count; i++) {
        fprintf(log, "%-8s rva=0x%08lX size=%9lu sha256=%s\n",
                info[i].name, (unsigned long)info[i].rva,
                (unsigned long)info[i].size, info[i].sha256);
    }
    fclose(log);
}

static void dump_sections(void) {
    FILE *log;
    char log_path[MAX_PATH];

    _snprintf(log_path, sizeof(log_path) - 1, "%sverifydump.txt", g_dir);
    log = fopen(log_path, "w");
    if (log) {
        fprintf(log, "verifydump - read only section dump of the loaded game modules\n");
        fprintf(log, "note: this module installs no hooks and patches no memory.\n");
        fclose(log);
    }
    dump_one_module("bm2dx.dll", "");
    dump_one_module("d3d9.dll", "d3d9_");
}


DWORD WINAPI verify_thread(LPVOID param) {
    int i;
    (void)param;
    for (i = 0; i < 60000; i++) {
        if (GetModuleHandleA("bm2dx.dll")) {
            break;
        }
        Sleep(1);
    }
    /* give the game time to finish its boot, create the device and let every
       other module install its hooks, so the dump reflects the steady state */
    Sleep(12000);
    dump_sections();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t = CreateThread(NULL, 0, verify_thread, NULL, 0, NULL);
        if (t) {
            CloseHandle(t);
        }
    }
    return TRUE;
}
