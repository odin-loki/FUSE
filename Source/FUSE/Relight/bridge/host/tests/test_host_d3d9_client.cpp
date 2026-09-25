// FUSE Relight RL-2.3: host-crash test on real D3D9 (Wine + Xvfb + Lavapipe).
// Copyright (c) 2026 FUSE contributors (AGPL-3.0).
//
// A test-local client (RL-2.2's proxy is written concurrently): it owns a window, launches
// fuse_relight_host.exe (vendored x64 DXVK + Relight) through BridgeLink and renders frames with
// commands built by the generated encoders: a triangle whose vertex colour changes every frame
// (dynamic VB, D3DLOCK_DISCARD) over a per-frame clear colour. Every frame reads the back buffer
// back (GetRenderTargetData) and checks one pixel inside the triangle and one outside.
//
//   test_host_d3d9_client.exe none|kill|kill-async|crash|hang|nohost [--host <x64 host.exe> --host-d3d9 <x64 d3d9.dll>]
// Exit 77 (skip) when this architecture has no Vulkan device (the fallback runs DXVK in-process).
//
// kill: the client kills the host after frame 10 (mid-run); kill-async: another thread kills it once
// frame 10 starts, wherever the command stream is; crash / hang: the host faults at its 10th Present. In every failure scenario the link must
// fall back to in-process plain DXVK (the passthrough backend, FUSE_RELIGHT=0) and every remaining
// frame must still render correctly: the app completes with exit code 0.
//
// The passthrough d3d9.dll is the vendored DXVK of this executable's architecture, next to it (the
// i686 tree: the x86 build of vendored DXVK, with the x64 tree's host).

#include <fuse/relight/bridge/host/d3d9_executor.hpp>
#include <fuse/relight/bridge/host/link.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace fuse::relight::bridge;
namespace cmd = schema::cmd;

namespace {

constexpr uint32_t kWidth = 128, kHeight = 96;
constexpr uint32_t kD3D = 1, kDevice = 2, kBackBuffer = 3, kReadback = 4, kVB = 5;
constexpr int kFrames = 30, kFaultFrame = 10;

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

HWND createWindow() {
    WNDCLASSA wc {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "rl_bridge_host_d3d9";
    RegisterClassA(&wc);
    RECT r {0, 0, LONG(kWidth), LONG(kHeight)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowA(wc.lpszClassName, "rl_bridge_host_d3d9", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOW);
    return h;
}

void pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

// Can this process's architecture reach a Vulkan device? (The passthrough fallback runs DXVK
// in-process; an x86 client needs a 32-bit Vulkan driver.) Minimal declarations: no Vulkan headers.
bool vulkanDeviceAvailable() {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (vk == nullptr) {
        return false;
    }
    struct InstanceCreateInfo {
        uint32_t sType;  // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
        const void* pNext;
        uint32_t flags;
        const void* pApplicationInfo;
        uint32_t enabledLayerCount;
        const char* const* ppEnabledLayerNames;
        uint32_t enabledExtensionCount;
        const char* const* ppEnabledExtensionNames;
    };
    using CreateInstance = int32_t(__stdcall*)(const InstanceCreateInfo*, const void*, void**);
    using EnumerateDevices = int32_t(__stdcall*)(void*, uint32_t*, void**);
    using DestroyInstance = void(__stdcall*)(void*, const void*);
    auto create = reinterpret_cast<CreateInstance>(reinterpret_cast<void (*)(void)>(GetProcAddress(vk, "vkCreateInstance")));
    auto enumerate =
        reinterpret_cast<EnumerateDevices>(reinterpret_cast<void (*)(void)>(GetProcAddress(vk, "vkEnumeratePhysicalDevices")));
    auto destroy = reinterpret_cast<DestroyInstance>(reinterpret_cast<void (*)(void)>(GetProcAddress(vk, "vkDestroyInstance")));
    if (create == nullptr || enumerate == nullptr || destroy == nullptr) {
        return false;
    }
    InstanceCreateInfo ci {1, nullptr, 0, nullptr, 0, nullptr, 0, nullptr};
    void* instance = nullptr;
    if (create(&ci, nullptr, &instance) != 0 || instance == nullptr) {
        return false;
    }
    uint32_t count = 0;
    const bool ok = enumerate(instance, &count, nullptr) == 0 && count != 0;
    destroy(instance, nullptr);
    return ok;
}

uint32_t clearColour(int f) { return 0xFF000000u | (uint32_t(20 + f * 7) << 16) | (uint32_t(200 - f * 5) << 8) | 40u; }
uint32_t triColour(int f) { return 0xFF000000u | (uint32_t(250 - f * 3) << 16) | (uint32_t(30 + f * 6) << 8) | uint32_t(f * 8); }

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string scenario = argc > 1 ? argv[1] : "none";
    const std::string dir = host::thisModuleDirectory();
    const std::string d3d9 = dir + "d3d9.dll";  // this architecture's vendored DXVK: the passthrough
    // The host is x64 (--host / --host-d3d9 point a 32-bit client at the x64 tree's host).
    std::string hostExe = dir + "fuse_relight_host.exe";
    std::string hostD3d9 = d3d9;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0) {
            hostExe = argv[++i];
        } else if (std::strcmp(argv[i], "--host-d3d9") == 0) {
            hostD3d9 = argv[++i];
        }
    }
    // `none` never falls back, so it runs even without a Vulkan device of this architecture.
    if (scenario != "none" && !vulkanDeviceAvailable()) {
        std::printf("SKIP: no Vulkan device for this %u-bit process (the in-process DXVK fallback cannot run)\n",
                    unsigned(sizeof(void*) * 8));
        return 77;
    }
    if (scenario != "nohost" && GetFileAttributesA(hostExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("SKIP: host %s not built\n", hostExe.c_str());
        return 77;
    }
    HWND hwnd = createWindow();

    host::LinkConfig lc;
    lc.hangTimeoutMs = scenario == "hang" ? 4000 : 60000;
    lc.startupTimeoutMs = 60000;
    host::BridgeLink link(lc, host::makePassthroughFactory(d3d9));
    host::HostLaunch hl;
    hl.hostExe = scenario == "nohost" ? dir + "fuse_relight_host_missing.exe" : hostExe;
    hl.extraArgs = {"--d3d9", hostD3d9, "--relight", "1"};
    if (scenario == "crash") {
        hl.extraArgs.insert(hl.extraArgs.end(), {"--test-fault", "crash@" + std::to_string(kFaultFrame)});
    } else if (scenario == "hang") {
        hl.extraArgs.insert(hl.extraArgs.end(), {"--test-fault", "hang@" + std::to_string(kFaultFrame)});
    }
    const ipc::Result lr = link.launch(hl);
    std::printf("launch: %s -> %s %s\n", ipc::toString(lr), host::toString(link.mode()), link.lastError().c_str());

    std::atomic<bool> stop {false};
    std::atomic<int> frameNo {0};
    std::atomic<bool> killed {false};
    std::thread killer;
    if (scenario == "kill-async") {
        // Kill from another thread as soon as frame 10 starts, i.e. at an arbitrary point of the
        // command stream (progress-driven: a wall-clock delay raced the run's own length).
        killer = std::thread([&] {
            while (!stop && frameNo.load() < kFaultFrame) {
                Sleep(0);
            }
            if (!stop && link.hostProcess() != nullptr) {
                std::printf("killing the host from another thread during frame %d\n", frameNo.load());
                link.hostProcess()->kill();
            }
            killed = true;
        });
    }

    int failures = 0;
    auto expectHr = [&](int32_t hr, const char* what) {
        if (hr != host::kHrOk) {
            ++failures;
            std::fprintf(stderr, "FAIL: %s -> 0x%08x (%s)\n", what, static_cast<uint32_t>(hr), host::toString(link.mode()));
        }
    };

    // ---- setup ---------------------------------------------------------------------------------
    cmd::Direct3DCreate9 create;
    create.sdkVersion = D3D_SDK_VERSION;
    create.ex = 0;
    create.result = kD3D;
    expectHr(link.call(create), "Direct3DCreate9");
    cmd::IDirect3D9Ex_CreateDevice cd;
    cd.adapter = 0;
    cd.deviceType = D3DDEVTYPE_HAL;
    cd.focusWindow = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd));
    cd.behaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
    // D3DPRESENT_PARAMETERS in declaration order (RL-2.2 wire: u32[14]).
    cd.presentParameters = {kWidth, kHeight, uint32_t(D3DFMT_X8R8G8B8), 1u, uint32_t(D3DMULTISAMPLE_NONE), 0u,
                            uint32_t(D3DSWAPEFFECT_DISCARD), cd.focusWindow, uint32_t(TRUE), uint32_t(FALSE),
                            uint32_t(D3DFMT_UNKNOWN), 0u, 0u, uint32_t(D3DPRESENT_INTERVAL_IMMEDIATE)};
    cd.result = kDevice;
    host::Response created;
    expectHr(link.call(cd, kD3D, &created), "CreateDevice");

    cmd::IDirect3DDevice9Ex_LinkBackBuffer bb;
    bb.surface = kBackBuffer;
    link.call(bb, kDevice);
    cmd::IDirect3DDevice9Ex_CreateOffscreenPlainSurface rs;
    rs.width = kWidth;
    rs.height = kHeight;
    rs.format = D3DFMT_X8R8G8B8;
    rs.pool = D3DPOOL_SYSTEMMEM;
    rs.result = kReadback;
    expectHr(link.call(rs, kDevice), "CreateOffscreenPlainSurface");
    cmd::IDirect3DDevice9Ex_CreateVertexBuffer vb;
    vb.length = 3 * 20;
    vb.usage = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
    vb.fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    vb.pool = D3DPOOL_DEFAULT;
    vb.result = kVB;
    expectHr(link.call(vb, kDevice), "CreateVertexBuffer");
    auto setRs = [&](uint32_t s, uint32_t v) {
        cmd::IDirect3DDevice9Ex_SetRenderState c;
        c.state = s;
        c.value = v;
        link.call(c, kDevice);
    };
    setRs(D3DRS_LIGHTING, FALSE);
    setRs(D3DRS_CULLMODE, D3DCULL_NONE);
    setRs(D3DRS_ZENABLE, D3DZB_FALSE);
    cmd::IDirect3DDevice9Ex_SetFVF fvf;
    fvf.fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    link.call(fvf, kDevice);
    cmd::IDirect3DDevice9Ex_SetStreamSource ss;
    ss.streamNumber = 0;
    ss.vertexBuffer = kVB;
    ss.stride = 20;
    link.call(ss, kDevice);

    // ---- frames --------------------------------------------------------------------------------
    int verified = 0;
    for (int f = 0; f < kFrames; ++f) {
        frameNo = f;
        // kill-async: frames 10..13 race the kill; frame 14 waits until it was issued.
        for (int w = 0; scenario == "kill-async" && f == kFaultFrame + 4 && !killed && w < 5000; ++w) {
            Sleep(1);
        }
        pump();
        if (scenario == "kill" && f == kFaultFrame && link.hostProcess() != nullptr) {
            std::printf("killing the host before frame %d\n", f);
            link.hostProcess()->kill();
        }
        struct V {
            float x, y, z, rhw;
            uint32_t c;
        };
        const V tri[3] = {{-10.f, -10.f, 0.5f, 1.f, triColour(f)}, {150.f, -10.f, 0.5f, 1.f, triColour(f)},
                          {-10.f, 110.f, 0.5f, 1.f, triColour(f)}};
        // The client locks its shadow copy (write-only: no read-back); Unlock uploads it.
        cmd::IDirect3DVertexBuffer9_Unlock unlock;
        unlock.offset = 0;
        unlock.flags = D3DLOCK_DISCARD;
        unlock.data.resize(sizeof(tri));
        std::memcpy(unlock.data.data(), tri, sizeof(tri));
        link.call(unlock, kVB);

        cmd::IDirect3DDevice9Ex_Clear clear;
        clear.flags = D3DCLEAR_TARGET;
        clear.color = clearColour(f);
        clear.z = 1.f;
        link.call(clear, kDevice);
        link.call(cmd::IDirect3DDevice9Ex_BeginScene {}, kDevice);
        cmd::IDirect3DDevice9Ex_DrawPrimitive draw;
        draw.primitiveType = D3DPT_TRIANGLELIST;
        draw.primitiveCount = 1;
        link.call(draw, kDevice);
        link.call(cmd::IDirect3DDevice9Ex_EndScene {}, kDevice);

        cmd::IDirect3DDevice9Ex_GetRenderTargetData rt;
        rt.renderTarget = kBackBuffer;
        rt.destination = kReadback;
        host::Response resp;
        const int32_t hr = link.call(rt, kDevice, &resp);
        bool ok = false;
        // Reply_Data: the destination surface, tightly packed rows (128 x 4 bytes).
        if (hr == host::kHrOk && resp.payload.size() == size_t(kWidth) * kHeight * 4) {
            auto px = [&](uint32_t x, uint32_t y) {
                uint32_t v = 0;
                std::memcpy(&v, resp.payload.data() + (size_t(y) * kWidth + x) * 4, 4);
                return v & 0x00FFFFFFu;
            };
            const uint32_t in = px(20, 20), out = px(120, 90);
            ok = in == (triColour(f) & 0x00FFFFFFu) && out == (clearColour(f) & 0x00FFFFFFu);
            if (!ok) {
                std::fprintf(stderr, "frame %d (%s): inside %06x want %06x, outside %06x want %06x\n", f,
                             host::toString(link.mode()), in, triColour(f) & 0xFFFFFFu, out, clearColour(f) & 0xFFFFFFu);
            }
        } else {
            std::fprintf(stderr, "frame %d (%s): readback failed 0x%08x (%zu bytes)\n", f, host::toString(link.mode()),
                         static_cast<uint32_t>(hr), resp.payload.size());
        }
        verified += ok ? 1 : 0;
        cmd::IDirect3DDevice9Ex_Present present;
        link.call(present, kDevice);
    }
    stop = true;
    if (killer.joinable()) {
        killer.join();
    }
    const host::LinkMode mode = link.mode();
    link.shutdown();
    int hostExit = -1;
    if (link.hostProcess() != nullptr) {
        link.hostProcess()->wait(10000, &hostExit);
    }
    std::printf("scenario %s: %d/%d frames verified, mode %s, %u fallback(s) (%s), %llu commands replayed in %llu ms, "
                "host exit %d\n",
                scenario.c_str(), verified, kFrames, host::toString(mode), link.fallbacks(), host::toString(link.reason()),
                static_cast<unsigned long long>(link.replayedCommands()),
                static_cast<unsigned long long>(link.fallbackMs()), hostExit);
    if (verified != kFrames) {
        ++failures;
    }
    if (scenario == "none") {
        if (mode != host::LinkMode::Bridged || link.fallbacks() != 0 || hostExit != host::kHostExitOk) {
            ++failures;
            std::fprintf(stderr, "FAIL: expected a clean bridged run\n");
        }
    } else if (mode != host::LinkMode::Passthrough || link.fallbacks() != 1) {
        ++failures;
        std::fprintf(stderr, "FAIL: expected exactly one fallback to passthrough\n");
    }
    DestroyWindow(hwnd);
    std::printf("rl_bridge_host_d3d9_%s: %s\n", scenario.c_str(), failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
