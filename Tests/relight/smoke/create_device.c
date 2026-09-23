/* RL-0.2 smoke test for the vendored DXVK d3d9.dll (docs/plans/FUSE_REMIX_PORT_PLAN.md §7 RL-0.2).
 * Copyright (c) 2026 FUSE contributors (MIT).
 *
 * Creates a D3D9 HAL device on a 128x96 window, renders three frames (full clear, then a partial
 * clear with a D3DRECT), reads the back buffer back with GetRenderTargetData before the last
 * Present, and checks every pixel. It also checks that the d3d9.dll in the process is the one next
 * to this exe (ours), not Wine's builtin; set RL_SMOKE_ALLOW_SYSTEM_D3D9=1 to run against another
 * d3d9 as a reference.
 *
 * Exit codes: 0 pass, 1 fail. Built by Source/FUSE/Relight/cmake/relight_dxvk.cmake and run by
 * run_smoke.sh (ctest rl_dxvk_smoke) under Xvfb + Wine + Lavapipe.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RL_W 128
#define RL_H 96
#define RL_FRAMES 3

/* Last frame: whole target cleared to RL_BG, then the RL_RECT region cleared to RL_FG. */
#define RL_BG 0x002080e0u
#define RL_FG 0x00f04010u
static const D3DRECT RL_RECT = {32, 24, 96, 72};

static LRESULT CALLBACK rl_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int rl_fail(const char* what, HRESULT hr) {
    fprintf(stderr, "rl_smoke: FAIL %s (hr=0x%08lx)\n", what, (unsigned long)hr);
    return 1;
}

static void rl_dirname(char* path) {
    char* slash = strrchr(path, '\\');
    char* fwd = strrchr(path, '/');
    if (fwd != NULL && (slash == NULL || fwd > slash)) {
        slash = fwd;
    }
    if (slash != NULL) {
        *slash = '\0';
    }
}

/* The d3d9.dll the loader bound must come from this exe's directory. */
static int rl_check_module(void) {
    char exe[MAX_PATH];
    char dll[MAX_PATH];
    HMODULE mod = GetModuleHandleA("d3d9.dll");
    const char* allow = getenv("RL_SMOKE_ALLOW_SYSTEM_D3D9");
    if (mod == NULL || GetModuleFileNameA(NULL, exe, MAX_PATH) == 0 || GetModuleFileNameA(mod, dll, MAX_PATH) == 0) {
        return rl_fail("module lookup", HRESULT_FROM_WIN32(GetLastError()));
    }
    printf("rl_smoke: d3d9 module %s\n", dll);
    rl_dirname(exe);
    rl_dirname(dll);
    if (lstrcmpiA(exe, dll) != 0 && (allow == NULL || strcmp(allow, "1") != 0)) {
        fprintf(stderr, "rl_smoke: FAIL d3d9.dll is not the one next to the exe (%s vs %s)\n", dll, exe);
        return 1;
    }
    return 0;
}

static int rl_expect_pixels(const D3DLOCKED_RECT* lr) {
    unsigned bad = 0;
    for (int y = 0; y < RL_H; ++y) {
        const DWORD* row = (const DWORD*)((const BYTE*)lr->pBits + (size_t)y * (size_t)lr->Pitch);
        for (int x = 0; x < RL_W; ++x) {
            const int inside = x >= RL_RECT.x1 && x < RL_RECT.x2 && y >= RL_RECT.y1 && y < RL_RECT.y2;
            const DWORD want = inside ? RL_FG : RL_BG;
            const DWORD got = row[x] & 0x00ffffffu;
            if (got != want) {
                if (bad < 4) {
                    fprintf(stderr, "rl_smoke: pixel (%d,%d) = %06lx, expected %06lx\n", x, y,
                            (unsigned long)got, (unsigned long)want);
                }
                ++bad;
            }
        }
    }
    if (bad != 0) {
        fprintf(stderr, "rl_smoke: FAIL readback: %u of %d pixels wrong\n", bad, RL_W * RL_H);
        return 1;
    }
    return 0;
}

static int rl_readback(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = NULL;
    IDirect3DSurface9* sys = NULL;
    D3DLOCKED_RECT lr;
    int rc = 1;
    HRESULT hr = IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr)) {
        return rl_fail("GetBackBuffer", hr);
    }
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, RL_W, RL_H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (FAILED(hr)) {
        rc = rl_fail("CreateOffscreenPlainSurface", hr);
    } else if (FAILED(hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))) {
        rc = rl_fail("GetRenderTargetData", hr);
    } else if (FAILED(hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        rc = rl_fail("LockRect", hr);
    } else {
        rc = rl_expect_pixels(&lr);
        IDirect3DSurface9_UnlockRect(sys);
    }
    if (sys != NULL) {
        IDirect3DSurface9_Release(sys);
    }
    IDirect3DSurface9_Release(bb);
    return rc;
}

static int rl_run(HWND hwnd) {
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* dev = NULL;
    D3DADAPTER_IDENTIFIER9 id;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    int rc = 0;
    if (d3d == NULL) {
        return rl_fail("Direct3DCreate9", E_FAIL);
    }
    memset(&id, 0, sizeof(id));
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id))) {
        printf("rl_smoke: adapter \"%s\" (driver %s)\n", id.Description, id.Driver);
    }
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = RL_W;
    pp.BackBufferHeight = RL_H;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        IDirect3D9_Release(d3d);
        return rl_fail("CreateDevice", hr);
    }
    for (int frame = 0; frame < RL_FRAMES && rc == 0; ++frame) {
        const int last = frame == RL_FRAMES - 1;
        hr = IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, last ? RL_BG : 0x00ff00ffu, 1.0f, 0);
        if (SUCCEEDED(hr) && last) {
            hr = IDirect3DDevice9_Clear(dev, 1, &RL_RECT, D3DCLEAR_TARGET, RL_FG, 1.0f, 0);
        }
        if (FAILED(hr)) {
            rc = rl_fail("Clear", hr);
            break;
        }
        if (FAILED(hr = IDirect3DDevice9_BeginScene(dev)) || FAILED(hr = IDirect3DDevice9_EndScene(dev))) {
            rc = rl_fail("BeginScene/EndScene", hr);
            break;
        }
        if (last) {
            rc = rl_readback(dev);
        }
        if (rc == 0 && FAILED(hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL))) {
            rc = rl_fail("Present", hr);
        }
    }
    if (rc == 0 && FAILED(hr = IDirect3DDevice9_TestCooperativeLevel(dev))) {
        rc = rl_fail("TestCooperativeLevel", hr);
    }
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return rc;
}

int main(void) {
    WNDCLASSA wc;
    RECT r = {0, 0, RL_W, RL_H};
    HWND hwnd;
    int rc;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (rl_check_module() != 0) {
        return 1;
    }
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = rl_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "fuse_rl_smoke";
    if (RegisterClassA(&wc) == 0) {
        return rl_fail("RegisterClass", HRESULT_FROM_WIN32(GetLastError()));
    }
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, wc.lpszClassName, "FUSE Relight smoke", WS_OVERLAPPEDWINDOW, 0, 0,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (hwnd == NULL) {
        return rl_fail("CreateWindowEx", HRESULT_FROM_WIN32(GetLastError()));
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    rc = rl_run(hwnd);
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    if (rc == 0) {
        printf("rl_smoke: PASS (device, clear, present, readback %dx%d)\n", RL_W, RL_H);
    }
    return rc;
}
