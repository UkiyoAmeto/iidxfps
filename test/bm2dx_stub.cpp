/*
 * test stub that stands in for bm2dx.dll
 *
 * It imports exactly the two things iidxfps hooks (d3d9.dll!Direct3DCreate9Ex and
 * user32.dll!EnumDisplaySettingsA), creates a D3D9 device with vsync ENABLED
 * (PresentationInterval = D3DPRESENT_INTERVAL_ONE) and then runs the naive
 * "one logic tick per present" loop the real game uses.  The measured loop rate
 * tells us whether iidxfps manages to lock the frame rate.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

extern "C" __declspec(dllexport) int dll_entry_init(char *sid, void *param) {
    (void)sid; (void)param;
    return 1;
}

extern "C" __declspec(dllexport) void dll_entry_main(void) {
}

static HWND make_window(void) {
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "iidxfps_test";
    if (!RegisterClassExA(&wc)) {
        /* already registered is fine */
    }
    return CreateWindowExA(0, "iidxfps_test", "iidxfps test", WS_OVERLAPPEDWINDOW,
                           0, 0, 640, 480, NULL, NULL, GetModuleHandleA(NULL), NULL);
}

extern "C" __declspec(dllexport) double test_run(int seconds) {
    IDirect3D9Ex *d3d = NULL;
    IDirect3DDevice9Ex *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    HWND hwnd;
    DEVMODEA dm;
    LARGE_INTEGER freq, t0, t1;
    long frames = 0;
    double elapsed = 0.0;
    HRESULT hr;
    DWORD behave;

    hwnd = make_window();
    {
        char show[8];
        if (GetEnvironmentVariableA("IIDXFPS_TEST_SHOW", show, sizeof(show)) > 0) {
            ShowWindow(hwnd, SW_SHOW);
            UpdateWindow(hwnd);
        }
    }
    if (!hwnd) {
        printf("[stub] CreateWindowEx failed: %lu\n", GetLastError());
        return -1.0;
    }

    /* this call is what iidxfps hooks */
    hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d);
    if (FAILED(hr) || !d3d) {
        printf("[stub] Direct3DCreate9Ex failed: 0x%08lX\n", (unsigned long)hr);
        return -1.0;
    }

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.EnableAutoDepthStencil = FALSE;
    /* deliberately ask for vsync, iidxfps is supposed to override this */
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    pp.FullScreen_RefreshRateInHz = 0;

    behave = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, behave,
                             &pp, NULL, &dev);
    if (FAILED(hr)) {
        behave = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
        hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, behave,
                                 &pp, NULL, &dev);
    }
    if (FAILED(hr) || !dev) {
        printf("[stub] CreateDeviceEx failed: 0x%08lX\n", (unsigned long)hr);
        d3d->Release();
        return -1.0;
    }

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        printf("[stub] game sees monitor %lux%lu @ %lu Hz\n",
               (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
               (unsigned long)dm.dmDisplayFrequency);
    }
    printf("[stub] returned PresentationInterval = 0x%08lX\n",
           (unsigned long)pp.PresentationInterval);
    {
        void **vt = *(void ***)dev;
        printf("[stub] dev=%p vtable=%p  [17]=%p [41]=%p [42]=%p [43]=%p [121]=%p\n",
               (void *)dev, (void *)vt, vt[17], vt[41], vt[42], vt[43], vt[121]);
        fflush(stdout);
    }

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    {
        double next_report = 0.5;
        for (;;) {
            ((IDirect3DDevice9 *)dev)->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(16, 16, 16), 1.0f, 0);
            ((IDirect3DDevice9 *)dev)->BeginScene();
            ((IDirect3DDevice9 *)dev)->EndScene();
            ((IDirect3DDevice9 *)dev)->Present(NULL, NULL, NULL, NULL);
            frames++;
            QueryPerformanceCounter(&t1);
            elapsed = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
            if (elapsed >= next_report) {
                printf("[stub] t=%.2fs frames=%ld fps=%.1f\n", elapsed, frames, frames / elapsed);
                fflush(stdout);
                next_report += 0.5;
            }
            if (elapsed >= (double)seconds) {
                break;
            }
        }
    }

    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    printf("[stub] %ld frames in %.3fs\n", frames, elapsed);
    return (double)frames / elapsed;
}



