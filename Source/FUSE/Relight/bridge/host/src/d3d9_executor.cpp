/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix bridge/src/server/main.cpp@0867d3c (ProcessDeviceCommandQueue, InitializeD3D,
// getPresParamFromRaw, ReturnSurfaceDataToClient, safeDestroy)

// FUSE Relight RL-2.3: D3D9 command executor (see d3d9_executor.hpp for what changed).

#include <fuse/relight/bridge/host/d3d9_executor.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace fuse::relight::bridge::host {

namespace cmd = schema::cmd;

namespace dxvkabi {
// DXVK's D3D8 interop interfaces (Engine/lib/dxvk/src/d3d9/d3d9_bridge.h), declared by ABI so the
// executor does not include DXVK's internal headers: same GUIDs, same vtable order.
// They must have external linkage: in an anonymous namespace GCC sees that no class of the program
// implements them and devirtualizes the pure-virtual call into a jump to address 0 (-O2/-O3).
struct DeviceBridge : public IUnknown {
    virtual HRESULT UpdateTextureFromBuffer(IDirect3DSurface9* dst, IDirect3DSurface9* src, const RECT* srcRect,
                                            const POINT* dstPoint) = 0;
    virtual bool IsSupportedSurfaceFormat(D3DFORMAT format) = 0;
};
struct InterfaceBridge : public IUnknown {
    virtual void SetD3DCompatibility(uint8_t compatibility) const = 0;
    virtual const void* GetConfig() const = 0;
};
}  // namespace dxvkabi

namespace {

enum class Kind : uint8_t {
    Device,
    SwapChain,
    Texture,
    VolumeTexture,
    CubeTexture,
    VertexBuffer,
    IndexBuffer,
    Surface,
    Volume,
    VertexDeclaration,
    VertexShader,
    PixelShader,
    StateBlock,
    Query,
};

struct Obj {
    IUnknown* p = nullptr;
    Kind kind = Kind::Device;
    uint32_t refs = 0;
};

const GUID kDxvkDeviceBridgeIid = {0xD3D0D3D9, 0x42A9, 0x4C1E, {0xAA, 0x97, 0xBE, 0xEF, 0xCA, 0xFE, 0x20, 0x00}};
const GUID kDxvkInterfaceBridgeIid = {0xD3D0D3D9, 0xA407, 0x773E, {0x18, 0xE9, 0xCA, 0xFE, 0xBE, 0xEF, 0x30, 0x00}};

// 32-bit window handles from the client; the host sign-extends (Win32 handles are 32-bit values on
// WOW64; upstream TRUNCATE_HANDLE).
HWND toHwnd(uint32_t v) { return reinterpret_cast<HWND>(static_cast<intptr_t>(static_cast<int32_t>(v))); }

D3DPRESENT_PARAMETERS toPresentParameters(const std::array<uint32_t, 14>& w) {
    D3DPRESENT_PARAMETERS pp {};
    pp.BackBufferWidth = w[0];
    pp.BackBufferHeight = w[1];
    pp.BackBufferFormat = static_cast<D3DFORMAT>(w[2]);
    pp.BackBufferCount = w[3];
    pp.MultiSampleType = static_cast<D3DMULTISAMPLE_TYPE>(w[4]);
    pp.MultiSampleQuality = w[5];
    pp.SwapEffect = static_cast<D3DSWAPEFFECT>(w[6]);
    pp.hDeviceWindow = toHwnd(w[7]);
    pp.Windowed = static_cast<BOOL>(w[8]);
    pp.EnableAutoDepthStencil = static_cast<BOOL>(w[9]);
    pp.AutoDepthStencilFormat = static_cast<D3DFORMAT>(w[10]);
    pp.Flags = w[11];
    pp.FullScreen_RefreshRateInHz = w[12];
    pp.PresentationInterval = w[13];
    return pp;
}

// Counted i32/u32 arrays standing for optional structures (0 elements = NULL pointer).
template <class T, class E>
const T* optionalWords(const std::vector<E>& words, T& storage, bool& ok) {
    static_assert(sizeof(T) % 4 == 0 && sizeof(E) == 4, "word-sized structures only");
    if (words.empty()) {
        return nullptr;
    }
    if (words.size() * 4 != sizeof(T)) {
        ok = false;
        return nullptr;
    }
    std::memcpy(&storage, words.data(), sizeof(T));
    return &storage;
}

// Bytes per block and block size of a format, for tightly packed rows.
struct BlockInfo {
    uint32_t bytes = 0;  // 0: unknown format
    uint32_t dim = 1;
};

BlockInfo blockInfo(D3DFORMAT f) {
    switch (static_cast<uint32_t>(f)) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_R32F:
    case D3DFMT_G16R16F: case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16:
    case D3DFMT_A2W10V10U10: case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8:
        return {4, 1};
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_A8R3G3B2: case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_R16F:
    case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE:
        return {2, 1};
    case D3DFMT_R8G8B8:
        return {3, 1};
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2:
        return {1, 1};
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16:
        return {8, 1};
    case D3DFMT_A32B32G32R32F:
        return {16, 1};
    case D3DFMT_DXT1:
        return {8, 4};
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        return {16, 4};
    default:
        return {};
    }
}

// Tightly packed layout of a width x height region of `format` (block rows for BC formats).
void packedLayout(D3DFORMAT format, uint32_t width, uint32_t height, INT pitch, uint32_t& rowBytes, uint32_t& rows) {
    const BlockInfo bi = blockInfo(format);
    if (bi.bytes != 0) {
        rowBytes = ((width + bi.dim - 1) / bi.dim) * bi.bytes;
        rows = (height + bi.dim - 1) / bi.dim;
    } else {
        rowBytes = static_cast<uint32_t>(pitch);
        rows = height;
    }
}

void setProcessEnv(const char* name, const char* value) {
    // The CRT's getenv (used by the tap) keeps its own copy of the environment.
    _putenv_s(name, value);
    ::SetEnvironmentVariableA(name, value);
}

}  // namespace

struct D3D9Executor::Impl {
    HMODULE module = nullptr;
    using CreateFn = IDirect3D9*(WINAPI*)(UINT);
    using CreateExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    CreateFn createFn = nullptr;
    CreateExFn createExFn = nullptr;
    // IDirect3D9 objects by client handle (Direct3DCreate9: `ex` selects Direct3DCreate9Ex; DXVK
    // gives an Ex interface D3D9Ex semantics, e.g. no D3DPOOL_MANAGED, so a plain game must get a
    // plain interface). `def` serves IDirect3D9Ex_* rows whose handle is unknown.
    struct D3DObj {
        IDirect3D9* d3d = nullptr;
        IDirect3D9Ex* ex = nullptr;
    };
    std::unordered_map<uint32_t, D3DObj> d3ds;
    D3DObj def;
    // The interface the current call targets.
    IDirect3D9* d3d = nullptr;
    IDirect3D9Ex* d3dEx = nullptr;

    static void releaseD3D(D3DObj& o) {
        if (o.ex != nullptr) {
            o.ex->Release();
        }
        if (o.d3d != nullptr) {
            o.d3d->Release();
        }
        o = D3DObj {};
    }
    HRESULT createD3D(bool ex, UINT sdk, D3DObj& out) {
        out = D3DObj {};
        if (ex) {
            if (createExFn == nullptr) {
                return D3DERR_NOTAVAILABLE;
            }
            const HRESULT r = createExFn(sdk, &out.ex);
            if (FAILED(r) || out.ex == nullptr) {
                out = D3DObj {};
                return FAILED(r) ? r : D3DERR_NOTAVAILABLE;
            }
            out.d3d = out.ex;
            out.d3d->AddRef();
            return S_OK;
        }
        if (createFn == nullptr) {
            return D3DERR_NOTAVAILABLE;
        }
        out.d3d = createFn(sdk);
        return out.d3d != nullptr ? S_OK : D3DERR_NOTAVAILABLE;
    }
    // Selects the interface for an IDirect3D9Ex_* call (creating a plain default if none exists).
    bool selectD3D(uint32_t h) {
        auto it = d3ds.find(h);
        const D3DObj* o = it != d3ds.end() ? &it->second : nullptr;
        if (o == nullptr) {
            if (def.d3d == nullptr && FAILED(createD3D(false, D3D_SDK_VERSION, def))) {
                return false;
            }
            o = &def;
        }
        d3d = o->d3d;
        d3dEx = o->ex;
        return d3d != nullptr;
    }
    std::unordered_map<uint32_t, Obj> objs;
    ipc::SharedHeap* heap = nullptr;
    D3D9ExecutorStats stats;
    bool terminated = false;
    bool verbose = false;

    // Windows of other processes that could not host a Vulkan surface (Wine: winevulkan has no
    // cross-process surfaces) -> a window of this process that stands in for presentation.
    std::unordered_map<HWND, HWND> standIns;
    bool ownWindowFallback = false;

    static bool foreignWindow(HWND w) {
        DWORD pid = 0;
        return w != nullptr && ::GetWindowThreadProcessId(w, &pid) != 0 && pid != ::GetCurrentProcessId();
    }
    HWND mapWindow(HWND w) const {
        auto it = standIns.find(w);
        return it == standIns.end() ? w : it->second;
    }
    HWND standInFor(HWND w, UINT width, UINT height) {
        auto it = standIns.find(w);
        if (it != standIns.end()) {
            return it->second;
        }
        static bool registered = false;
        HINSTANCE inst = ::GetModuleHandleA(nullptr);
        if (!registered) {
            WNDCLASSA wc {};
            wc.lpfnWndProc = ::DefWindowProcA;
            wc.hInstance = inst;
            wc.lpszClassName = "fuse_relight_host_present";
            ::RegisterClassA(&wc);
            registered = true;
        }
        RECT rc {0, 0, LONG(width != 0 ? width : 640), LONG(height != 0 ? height : 480)};
        if (width == 0 || height == 0) {
            ::GetClientRect(w, &rc);
        }
        ::AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        HWND h = ::CreateWindowA("fuse_relight_host_present", "FUSE Relight", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left,
                                 rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
        if (h != nullptr) {
            ::ShowWindow(h, SW_SHOWNOACTIVATE);
            standIns[w] = h;
            std::fprintf(stderr,
                         "fuse-relight executor: cannot present into the client's window %p from this process; presenting "
                         "into a host window instead\n",
                         static_cast<void*>(w));
        }
        return h;
    }
    // Creates a device; when the client's window cannot host a surface here, retries on a stand-in.
    template <class Create>
    HRESULT createDevice(HWND focus, D3DPRESENT_PARAMETERS& pp, Create&& create) {
        pp.hDeviceWindow = mapWindow(pp.hDeviceWindow);
        focus = mapWindow(focus);
        HRESULT r = create(focus, pp);
        const HWND target = pp.hDeviceWindow != nullptr ? pp.hDeviceWindow : focus;
        if (FAILED(r) && ownWindowFallback && foreignWindow(target)) {
            HWND standIn = standInFor(target, pp.BackBufferWidth, pp.BackBufferHeight);
            if (standIn != nullptr) {
                if (pp.hDeviceWindow == target) {
                    pp.hDeviceWindow = standIn;
                }
                if (focus == target) {
                    focus = standIn;
                }
                if (pp.hDeviceWindow == nullptr) {
                    pp.hDeviceWindow = standIn;
                }
                r = create(focus, pp);
            }
        }
        return r;
    }

    // Per-call context.
    uint32_t handle = 0;
    HRESULT hr = S_OK;
    std::vector<uint8_t> payload;  // Reply_Data bytes, or an encoded typed reply
    bool bad = false;              // a field failed validation

    // ---- object table ------------------------------------------------------------------------
    Obj* find(uint32_t h) {
        auto it = objs.find(h);
        if (it == objs.end() || it->second.p == nullptr) {
            ++stats.missingHandles;
            return nullptr;
        }
        return &it->second;
    }

    // Takes ownership of one reference to p under handle h (replacing whatever was there).
    void link(uint32_t h, IUnknown* p, Kind k) {
        if (h == 0 || p == nullptr) {
            if (p != nullptr) {
                p->Release();
            }
            return;
        }
        auto it = objs.find(h);
        if (it != objs.end()) {
            if (it->second.p == p) {
                it->second.refs += 1;
                return;
            }
            releaseObj(it->second);
        }
        objs[h] = Obj {p, k, 1};
    }

    static void releaseObj(Obj& o) {
        for (uint32_t i = 0; i < o.refs && o.p != nullptr; ++i) {
            o.p->Release();
        }
        o.p = nullptr;
        o.refs = 0;
    }

    template <class T>
    T* as(uint32_t h, Kind k) {
        Obj* o = find(h);
        if (o == nullptr || o->kind != k) {
            if (o != nullptr) {
                ++stats.missingHandles;
            }
            hr = D3DERR_INVALIDCALL;
            return nullptr;
        }
        return static_cast<T*>(o->p);
    }
    // Optional object argument: handle 0 = NULL (not an error).
    template <class T>
    T* opt(uint32_t h, Kind k, bool& ok) {
        if (h == 0) {
            return nullptr;
        }
        T* p = as<T>(h, k);
        if (p == nullptr) {
            ok = false;
        }
        return p;
    }

    IDirect3DDevice9* dev() { return as<IDirect3DDevice9>(handle, Kind::Device); }
    IDirect3DDevice9Ex* devEx() {
        IDirect3DDevice9* d = dev();
        if (d == nullptr) {
            return nullptr;
        }
        IDirect3DDevice9Ex* ex = nullptr;
        if (FAILED(d->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex)))) {
            hr = D3DERR_INVALIDCALL;
            return nullptr;
        }
        ex->Release();  // the device keeps it alive; we only borrow the Ex view
        return ex;
    }

    IDirect3DBaseTexture9* baseTexture(uint32_t h, bool& ok) {
        if (h == 0) {
            return nullptr;
        }
        Obj* o = find(h);
        if (o != nullptr) {
            switch (o->kind) {
            case Kind::Texture: return static_cast<IDirect3DTexture9*>(o->p);
            case Kind::VolumeTexture: return static_cast<IDirect3DVolumeTexture9*>(o->p);
            case Kind::CubeTexture: return static_cast<IDirect3DCubeTexture9*>(o->p);
            default: break;
            }
        }
        ok = false;
        hr = D3DERR_INVALIDCALL;
        return nullptr;
    }
    IDirect3DBaseTexture9* baseTexture() {
        bool ok = true;
        return baseTexture(handle, ok);
    }

    IDirect3DResource9* resource() {
        Obj* o = find(handle);
        if (o != nullptr) {
            switch (o->kind) {
            case Kind::Texture: return static_cast<IDirect3DTexture9*>(o->p);
            case Kind::VolumeTexture: return static_cast<IDirect3DVolumeTexture9*>(o->p);
            case Kind::CubeTexture: return static_cast<IDirect3DCubeTexture9*>(o->p);
            case Kind::VertexBuffer: return static_cast<IDirect3DVertexBuffer9*>(o->p);
            case Kind::IndexBuffer: return static_cast<IDirect3DIndexBuffer9*>(o->p);
            case Kind::Surface: return static_cast<IDirect3DSurface9*>(o->p);
            default: break;
            }
        }
        hr = D3DERR_INVALIDCALL;
        return nullptr;
    }

    void destroy() {
        auto it = objs.find(handle);
        if (it == objs.end()) {
            ++stats.missingHandles;
            return;
        }
        releaseObj(it->second);
        objs.erase(it);
    }

    void check(HRESULT r) {
        hr = r;
        if (FAILED(r)) {
            ++stats.failedCalls;
        }
    }

    // ---- replies -----------------------------------------------------------------------------
    template <class R>
    void reply(R& r) {
        r.requestUid = 0;  // the host loop writes the request's uid
        r.hresult = static_cast<int32_t>(hr);
        payload.resize(cmd::encodedSize(r));
        ipc::WireWriter w(payload.data(), payload.size());
        cmd::encode(w, r);
    }
    void replyValue(uint32_t v) {
        cmd::Reply_Value r;
        r.value = v;
        reply(r);
    }
    void replyMode(const D3DDISPLAYMODE& m, uint32_t scanLineOrdering = 0, uint32_t rotation = 0) {
        cmd::Reply_DisplayMode r;
        r.width = m.Width;
        r.height = m.Height;
        r.refreshRate = m.RefreshRate;
        r.format = static_cast<uint32_t>(m.Format);
        r.scanLineOrdering = scanLineOrdering;
        r.rotation = rotation;
        reply(r);
    }
    void replyModeEx(const D3DDISPLAYMODEEX& m, uint32_t rotation) {
        cmd::Reply_DisplayMode r;
        r.width = m.Width;
        r.height = m.Height;
        r.refreshRate = m.RefreshRate;
        r.format = static_cast<uint32_t>(m.Format);
        r.scanLineOrdering = static_cast<uint32_t>(m.ScanLineOrdering);
        r.rotation = rotation;
        reply(r);
    }
    void replyRaster(const D3DRASTER_STATUS& rs) {
        cmd::Reply_RasterStatus r;
        r.inVBlank = static_cast<uint32_t>(rs.InVBlank);
        r.scanLine = rs.ScanLine;
        reply(r);
    }

    // Reply_Data: rows of a locked rect, tightly packed.
    void packRows(const void* bits, INT pitch, uint32_t rowBytes, uint32_t rows) {
        const size_t base = payload.size();
        payload.resize(base + size_t(rowBytes) * rows);
        const auto* src = static_cast<const uint8_t*>(bits);
        for (uint32_t y = 0; y < rows; ++y) {
            std::memcpy(payload.data() + base + size_t(y) * rowBytes, src + size_t(y) * static_cast<size_t>(pitch), rowBytes);
        }
    }

    void readSurface(IDirect3DSurface9* s, const RECT* rect) {
        if (FAILED(hr) || s == nullptr) {
            return;
        }
        D3DSURFACE_DESC desc {};
        HRESULT r = s->GetDesc(&desc);
        D3DLOCKED_RECT lr {};
        if (SUCCEEDED(r)) {
            r = s->LockRect(&lr, rect, D3DLOCK_READONLY);
        }
        if (FAILED(r)) {
            check(r);
            return;
        }
        const uint32_t w = rect ? uint32_t(rect->right - rect->left) : desc.Width;
        const uint32_t h = rect ? uint32_t(rect->bottom - rect->top) : desc.Height;
        uint32_t rowBytes = 0, rows = 0;
        packedLayout(desc.Format, w, h, lr.Pitch, rowBytes, rows);
        packRows(lr.pBits, lr.Pitch, rowBytes, rows);
        s->UnlockRect();
    }

    template <class Volume>
    void readVolume(Volume* v, const D3DBOX* box, UINT level) {
        D3DVOLUME_DESC desc {};
        HRESULT r;
        D3DLOCKED_BOX lb {};
        if constexpr (std::is_same_v<Volume, IDirect3DVolume9>) {
            (void) level;
            r = v->GetDesc(&desc);
            if (SUCCEEDED(r)) {
                r = v->LockBox(&lb, box, D3DLOCK_READONLY);
            }
        } else {
            r = v->GetLevelDesc(level, &desc);
            if (SUCCEEDED(r)) {
                r = v->LockBox(level, &lb, box, D3DLOCK_READONLY);
            }
        }
        if (FAILED(r)) {
            check(r);
            return;
        }
        const uint32_t w = box ? box->Right - box->Left : desc.Width;
        const uint32_t h = box ? box->Bottom - box->Top : desc.Height;
        const uint32_t d = box ? box->Back - box->Front : desc.Depth;
        uint32_t rowBytes = 0, rows = 0;
        packedLayout(desc.Format, w, h, lb.RowPitch, rowBytes, rows);
        for (uint32_t z = 0; z < d; ++z) {
            packRows(static_cast<const uint8_t*>(lb.pBits) + size_t(z) * static_cast<size_t>(lb.SlicePitch), lb.RowPitch,
                     rowBytes, rows);
        }
        if constexpr (std::is_same_v<Volume, IDirect3DVolume9>) {
            v->UnlockBox();
        } else {
            v->UnlockBox(level);
        }
    }

    template <class Buffer>
    void readBuffer(Buffer* b, uint32_t offset, uint32_t size) {
        if (b == nullptr) {
            return;
        }
        void* p = nullptr;
        HRESULT r = b->Lock(offset, size, &p, D3DLOCK_READONLY);
        if (FAILED(r)) {
            check(r);
            return;
        }
        payload.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + size);
        b->Unlock();
    }

    // ---- uploads -------------------------------------------------------------------------------
    struct Upload {
        Impl& im;
        UploadView v;
        bool ok;
        template <class C>
        Upload(Impl& i, const C& c) : im(i) {
            ok = resolveUpload(im.heap, c.heapChunk, c.heapBytes, c.data, v);
            if (!ok) {
                im.bad = true;
            }
        }
        ~Upload() { releaseUpload(im.heap, v); }
    };

    // Copies `rows` rows of `rowBytes` (packed) into a locked rect.
    static void unpackRows(void* bits, INT pitch, const uint8_t* src, size_t srcSize, uint32_t rowBytes, uint32_t rows) {
        const size_t n = std::min<size_t>(rows, rowBytes != 0 ? srcSize / rowBytes : 0);
        const size_t copy = std::min<size_t>(rowBytes, static_cast<size_t>(pitch));
        for (size_t y = 0; y < n; ++y) {
            std::memcpy(static_cast<uint8_t*>(bits) + y * static_cast<size_t>(pitch), src + y * rowBytes, copy);
        }
    }

    template <class C>
    void unlockSurface(IDirect3DSurface9* s, const C& c) {
        Upload up(*this, c);
        if (s == nullptr || !up.ok || up.v.size == 0) {
            return;
        }
        bool ok = true;
        RECT rc {};
        const RECT* pr = optionalWords(c.rect, rc, ok);
        if (!ok) {
            bad = true;
            return;
        }
        D3DLOCKED_RECT lr {};
        HRESULT r = s->LockRect(&lr, pr, c.flags & ~static_cast<DWORD>(D3DLOCK_READONLY));
        if (FAILED(r)) {
            check(r);
            return;
        }
        unpackRows(lr.pBits, lr.Pitch, up.v.data, up.v.size, c.rowBytes, c.rows);
        check(s->UnlockRect());
    }

    template <class Volume, class C>
    void unlockVolume(Volume* v, UINT level, const C& c) {
        Upload up(*this, c);
        if (v == nullptr || !up.ok || up.v.size == 0) {
            return;
        }
        bool ok = true;
        D3DBOX bx {};
        const D3DBOX* pb = optionalWords(c.box, bx, ok);
        if (!ok) {
            bad = true;
            return;
        }
        D3DLOCKED_BOX lb {};
        const DWORD flags = c.flags & ~static_cast<DWORD>(D3DLOCK_READONLY);
        HRESULT r;
        if constexpr (std::is_same_v<Volume, IDirect3DVolume9>) {
            (void) level;
            r = v->LockBox(&lb, pb, flags);
        } else {
            r = v->LockBox(level, &lb, pb, flags);
        }
        if (FAILED(r)) {
            check(r);
            return;
        }
        const size_t slice = size_t(c.rowBytes) * c.rows;
        for (uint32_t z = 0; z < c.slices && slice != 0 && (z + 1) * slice <= up.v.size; ++z) {
            unpackRows(static_cast<uint8_t*>(lb.pBits) + size_t(z) * static_cast<size_t>(lb.SlicePitch), lb.RowPitch,
                       up.v.data + z * slice, slice, c.rowBytes, c.rows);
        }
        if constexpr (std::is_same_v<Volume, IDirect3DVolume9>) {
            check(v->UnlockBox());
        } else {
            check(v->UnlockBox(level));
        }
    }

    template <class Buffer, class C>
    void unlockBuffer(Buffer* b, const C& c) {
        Upload up(*this, c);
        if (b == nullptr || !up.ok || up.v.size == 0) {
            return;
        }
        void* p = nullptr;
        HRESULT r = b->Lock(c.offset, static_cast<UINT>(up.v.size), &p, c.flags & ~static_cast<DWORD>(D3DLOCK_READONLY));
        if (FAILED(r)) {
            check(r);
            return;
        }
        std::memcpy(p, up.v.data, up.v.size);
        check(b->Unlock());
    }

    // ---- fallback for rows the executor does not implement -------------------------------------
    template <class C>
    void on(const C&) {
        ++stats.unhandled;
        ++stats.unhandledById[static_cast<uint16_t>(C::kId)];
        hr = E_NOTIMPL;
    }

    // ---- Bridge ----------------------------------------------------------------------------------
    void on(const cmd::Bridge_Terminate&) { terminated = true; }
    void on(const cmd::Bridge_Sync&) {}  // a fence: every earlier command has run (single thread)
    void on(const cmd::Bridge_WindowMessage&) {}  // no host-side UI consumes game input yet (RL-6.x)
    void on(const cmd::Bridge_DebugMessage& c) {
        std::fprintf(stderr, "fuse-relight bridge [client %u]: %s\n", c.level, c.text.c_str());
    }
    void on(const cmd::Bridge_UnlinkResource& c) { objs.erase(c.resource); }
    void on(const cmd::Bridge_UnlinkVolumeResource& c) { objs.erase(c.resource); }

    // ---- entry points (the executor's single IDirect3D9 object serves every handle) ------------
    void on(const cmd::Direct3DCreate9& c) {
        D3DObj o;
        hr = createD3D(c.ex != 0, c.sdkVersion != 0 ? c.sdkVersion : D3D_SDK_VERSION, o);
        if (FAILED(hr) || c.result == 0) {
            releaseD3D(o);
            return;
        }
        auto it = d3ds.find(c.result);
        if (it != d3ds.end()) {
            releaseD3D(it->second);
        }
        d3ds[c.result] = o;
    }
    void on(const cmd::IDirect3D9Ex_SetD3DCompatibility& c) {
        dxvkabi::InterfaceBridge* b = nullptr;
        if (SUCCEEDED(d3d->QueryInterface(kDxvkInterfaceBridgeIid, reinterpret_cast<void**>(&b))) && b != nullptr) {
            b->SetD3DCompatibility(static_cast<uint8_t>(c.compatibility));
            b->Release();
        } else {
            hr = E_NOINTERFACE;
        }
    }
    dxvkabi::DeviceBridge* deviceBridge() {
        IDirect3DDevice9* d = dev();
        dxvkabi::DeviceBridge* b = nullptr;
        if (d == nullptr || FAILED(d->QueryInterface(kDxvkDeviceBridgeIid, reinterpret_cast<void**>(&b)))) {
            hr = E_NOINTERFACE;
            return nullptr;
        }
        return b;
    }
    void on(const cmd::IDirect3DDevice9Ex_UpdateTextureFromBuffer& c) {
        if (dxvkabi::DeviceBridge* b = deviceBridge()) {
            bool ok = true;
            auto* dst = as<IDirect3DSurface9>(c.destination, Kind::Surface);
            auto* src = as<IDirect3DSurface9>(c.source, Kind::Surface);
            RECT r {};
            POINT pt {};
            const RECT* pr = optionalWords(c.sourceRect, r, ok);
            const POINT* pp = optionalWords(c.destPoint, pt, ok);
            if (!ok) {
                bad = true;
            } else if (dst != nullptr && src != nullptr) {
                check(b->UpdateTextureFromBuffer(dst, src, pr, pp));
            }
            b->Release();
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_IsSupportedSurfaceFormat& c) {
        uint32_t v = 0;
        if (dxvkabi::DeviceBridge* b = deviceBridge()) {
            v = b->IsSupportedSurfaceFormat(static_cast<D3DFORMAT>(c.format)) ? 1 : 0;
            b->Release();
        }
        replyValue(v);
    }

    // ---- IDirect3D9Ex ----------------------------------------------------------------------------
    void on(const cmd::IDirect3D9Ex_Destroy&) {
        auto it = d3ds.find(handle);
        if (it != d3ds.end()) {
            releaseD3D(it->second);  // devices keep their own reference to it
            d3ds.erase(it);
        }
    }
    void on(const cmd::IDirect3D9Ex_GetAdapterCount&) { replyValue(d3d->GetAdapterCount()); }
    void on(const cmd::IDirect3D9Ex_GetAdapterIdentifier& c) {
        D3DADAPTER_IDENTIFIER9 id {};
        check(d3d->GetAdapterIdentifier(c.adapter, c.flags, &id));
        cmd::Reply_AdapterIdentifier r;
        r.driver = id.Driver;
        r.description = id.Description;
        r.deviceName = id.DeviceName;
        r.driverVersion = static_cast<uint64_t>(id.DriverVersion.QuadPart);
        r.vendorId = id.VendorId;
        r.deviceId = id.DeviceId;
        r.subSysId = id.SubSysId;
        r.revision = id.Revision;
        std::memcpy(r.deviceIdentifier.data(), &id.DeviceIdentifier, 16);
        r.whqlLevel = id.WHQLLevel;
        reply(r);
    }
    void on(const cmd::IDirect3D9Ex_GetAdapterModeCount& c) {
        replyValue(d3d->GetAdapterModeCount(c.adapter, static_cast<D3DFORMAT>(c.format)));
    }
    void on(const cmd::IDirect3D9Ex_EnumAdapterModes& c) {
        D3DDISPLAYMODE m {};
        check(d3d->EnumAdapterModes(c.adapter, static_cast<D3DFORMAT>(c.format), c.mode, &m));
        replyMode(m);
    }
    void on(const cmd::IDirect3D9Ex_GetAdapterDisplayMode& c) {
        D3DDISPLAYMODE m {};
        check(d3d->GetAdapterDisplayMode(c.adapter, &m));
        replyMode(m);
    }
    void on(const cmd::IDirect3D9Ex_CheckDeviceType& c) {
        hr = d3d->CheckDeviceType(c.adapter, static_cast<D3DDEVTYPE>(c.deviceType), static_cast<D3DFORMAT>(c.adapterFormat),
                                  static_cast<D3DFORMAT>(c.backBufferFormat), static_cast<BOOL>(c.windowed));
    }
    void on(const cmd::IDirect3D9Ex_CheckDeviceFormat& c) {
        hr = d3d->CheckDeviceFormat(c.adapter, static_cast<D3DDEVTYPE>(c.deviceType), static_cast<D3DFORMAT>(c.adapterFormat),
                                    c.usage, static_cast<D3DRESOURCETYPE>(c.resourceType), static_cast<D3DFORMAT>(c.checkFormat));
    }
    void on(const cmd::IDirect3D9Ex_CheckDeviceMultiSampleType& c) {
        DWORD levels = 0;
        hr = d3d->CheckDeviceMultiSampleType(c.adapter, static_cast<D3DDEVTYPE>(c.deviceType),
                                             static_cast<D3DFORMAT>(c.surfaceFormat), static_cast<BOOL>(c.windowed),
                                             static_cast<D3DMULTISAMPLE_TYPE>(c.multiSampleType), &levels);
        replyValue(levels);
    }
    void on(const cmd::IDirect3D9Ex_CheckDepthStencilMatch& c) {
        hr = d3d->CheckDepthStencilMatch(c.adapter, static_cast<D3DDEVTYPE>(c.deviceType),
                                         static_cast<D3DFORMAT>(c.adapterFormat), static_cast<D3DFORMAT>(c.renderTargetFormat),
                                         static_cast<D3DFORMAT>(c.depthStencilFormat));
    }
    void on(const cmd::IDirect3D9Ex_CheckDeviceFormatConversion& c) {
        hr = d3d->CheckDeviceFormatConversion(c.adapter, static_cast<D3DDEVTYPE>(c.deviceType),
                                              static_cast<D3DFORMAT>(c.sourceFormat), static_cast<D3DFORMAT>(c.targetFormat));
    }
    void on(const cmd::IDirect3D9Ex_GetDeviceCaps& c) {
        D3DCAPS9 caps {};
        check(d3d->GetDeviceCaps(c.adapter, static_cast<D3DDEVTYPE>(c.deviceType), &caps));
        cmd::Reply_Caps r;
        static_assert(sizeof(caps) == sizeof(r.caps), "D3DCAPS9 is 76 words");
        std::memcpy(r.caps.data(), &caps, sizeof(caps));
        reply(r);
    }
    void on(const cmd::IDirect3D9Ex_GetAdapterMonitor& c) {
        replyValue(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(d3d->GetAdapterMonitor(c.adapter))));
    }
    void on(const cmd::IDirect3D9Ex_CreateDevice& c) {
        D3DPRESENT_PARAMETERS pp = toPresentParameters(c.presentParameters);
        IDirect3DDevice9* d = nullptr;
        check(createDevice(toHwnd(c.focusWindow), pp, [&](HWND focus, D3DPRESENT_PARAMETERS& p) {
            return d3d->CreateDevice(c.adapter, static_cast<D3DDEVTYPE>(c.deviceType), focus, c.behaviorFlags, &p, &d);
        }));
        if (SUCCEEDED(hr)) {
            link(c.result, d, Kind::Device);
        } else {
            std::fprintf(stderr, "fuse-relight executor: CreateDevice failed (0x%08lx)\n", static_cast<unsigned long>(hr));
        }
    }
    void on(const cmd::IDirect3D9Ex_CreateDeviceEx& c) {
        if (d3dEx == nullptr) {
            hr = D3DERR_NOTAVAILABLE;
            return;
        }
        bool ok = true;
        D3DPRESENT_PARAMETERS pp = toPresentParameters(c.presentParameters);
        D3DDISPLAYMODEEX mode {};
        const D3DDISPLAYMODEEX* pm = optionalWords(c.fullscreenDisplayMode, mode, ok);
        if (!ok) {
            bad = true;
            return;
        }
        IDirect3DDevice9Ex* d = nullptr;
        check(createDevice(toHwnd(c.focusWindow), pp, [&](HWND focus, D3DPRESENT_PARAMETERS& p) {
            return d3dEx->CreateDeviceEx(c.adapter, static_cast<D3DDEVTYPE>(c.deviceType), focus, c.behaviorFlags, &p,
                                         const_cast<D3DDISPLAYMODEEX*>(pm), &d);
        }));
        if (SUCCEEDED(hr)) {
            link(c.result, d, Kind::Device);
        }
    }
    void on(const cmd::IDirect3D9Ex_GetAdapterModeCountEx& c) {
        bool ok = true;
        D3DDISPLAYMODEFILTER f {};
        const D3DDISPLAYMODEFILTER* pf = optionalWords(c.filter, f, ok);
        replyValue(d3dEx != nullptr && ok ? d3dEx->GetAdapterModeCountEx(c.adapter, pf) : 0);
    }
    void on(const cmd::IDirect3D9Ex_EnumAdapterModesEx& c) {
        bool ok = true;
        D3DDISPLAYMODEFILTER f {};
        const D3DDISPLAYMODEFILTER* pf = optionalWords(c.filter, f, ok);
        D3DDISPLAYMODEEX m {};
        m.Size = sizeof(m);
        if (d3dEx == nullptr || !ok) {
            hr = D3DERR_INVALIDCALL;
        } else {
            check(d3dEx->EnumAdapterModesEx(c.adapter, pf, c.mode, &m));
        }
        replyModeEx(m, 0);
    }
    void on(const cmd::IDirect3D9Ex_GetAdapterDisplayModeEx& c) {
        D3DDISPLAYMODEEX m {};
        m.Size = sizeof(m);
        D3DDISPLAYROTATION rot = D3DDISPLAYROTATION_IDENTITY;
        if (d3dEx == nullptr) {
            hr = D3DERR_NOTAVAILABLE;
        } else {
            check(d3dEx->GetAdapterDisplayModeEx(c.adapter, &m, &rot));
        }
        replyModeEx(m, static_cast<uint32_t>(rot));
    }
    void on(const cmd::IDirect3D9Ex_GetAdapterLUID& c) {
        LUID luid {};
        if (d3dEx == nullptr) {
            hr = D3DERR_NOTAVAILABLE;
        } else {
            check(d3dEx->GetAdapterLUID(c.adapter, &luid));
        }
        cmd::Reply_Luid r;
        r.lowPart = luid.LowPart;
        r.highPart = luid.HighPart;
        reply(r);
    }

    // ---- bridge-only device links ----------------------------------------------------------------
    void on(const cmd::IDirect3DDevice9Ex_LinkSwapchain& c) {
        if (auto* d = dev()) {
            IDirect3DSwapChain9* sc = nullptr;
            check(d->GetSwapChain(c.index, &sc));
            link(c.swapChain, sc, Kind::SwapChain);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_LinkBackBuffer& c) {
        if (auto* d = dev()) {
            IDirect3DSurface9* s = nullptr;
            check(d->GetBackBuffer(c.swapChain, c.index, D3DBACKBUFFER_TYPE_MONO, &s));
            link(c.surface, s, Kind::Surface);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_LinkAutoDepthStencil& c) {
        if (auto* d = dev()) {
            IDirect3DSurface9* s = nullptr;
            check(d->GetDepthStencilSurface(&s));
            link(c.surface, s, Kind::Surface);
        }
    }

    // ---- IDirect3DDevice9Ex ----------------------------------------------------------------------
    void on(const cmd::IDirect3DDevice9Ex_Destroy&) { destroy(); }
    void on(const cmd::IDirect3DDevice9Ex_GetAvailableTextureMem&) {
        auto* d = dev();
        replyValue(d != nullptr ? d->GetAvailableTextureMem() : 0);
    }
    void on(const cmd::IDirect3DDevice9Ex_EvictManagedResources&) {
        if (auto* d = dev()) {
            check(d->EvictManagedResources());
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_GetDisplayMode& c) {
        D3DDISPLAYMODE m {};
        if (auto* d = dev()) {
            check(d->GetDisplayMode(c.swapChain, &m));
        }
        replyMode(m);
    }
    void on(const cmd::IDirect3DDevice9Ex_SetCursorProperties& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* s = opt<IDirect3DSurface9>(c.cursorBitmap, Kind::Surface, ok);
            if (ok) {
                check(d->SetCursorProperties(c.xHotSpot, c.yHotSpot, s));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetCursorPosition& c) {
        if (auto* d = dev()) {
            d->SetCursorPosition(c.x, c.y, c.flags);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_ShowCursor& c) {
        auto* d = dev();
        replyValue(d != nullptr ? static_cast<uint32_t>(d->ShowCursor(static_cast<BOOL>(c.show))) : 0);
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateAdditionalSwapChain& c) {
        if (auto* d = dev()) {
            D3DPRESENT_PARAMETERS pp = toPresentParameters(c.presentParameters);
            pp.hDeviceWindow = mapWindow(pp.hDeviceWindow);
            IDirect3DSwapChain9* sc = nullptr;
            check(d->CreateAdditionalSwapChain(&pp, &sc));
            link(c.result, sc, Kind::SwapChain);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_Reset& c) {
        if (auto* d = dev()) {
            D3DPRESENT_PARAMETERS pp = toPresentParameters(c.presentParameters);
            pp.hDeviceWindow = mapWindow(pp.hDeviceWindow);
            check(d->Reset(&pp));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_Present& c) {
        if (auto* d = dev()) {
            bool ok = true;
            RECT src {}, dst {};
            const RECT* ps = optionalWords(c.sourceRect, src, ok);
            const RECT* pd = optionalWords(c.destRect, dst, ok);
            if (!ok) {
                bad = true;
                return;
            }
            check(d->Present(ps, pd, mapWindow(toHwnd(c.destWindowOverride)), nullptr));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_GetRasterStatus& c) {
        D3DRASTER_STATUS rs {};
        if (auto* d = dev()) {
            check(d->GetRasterStatus(c.swapChain, &rs));
        }
        replyRaster(rs);
    }
    void on(const cmd::IDirect3DDevice9Ex_SetDialogBoxMode& c) {
        if (auto* d = dev()) {
            check(d->SetDialogBoxMode(static_cast<BOOL>(c.enable)));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetGammaRamp& c) {
        if (auto* d = dev()) {
            D3DGAMMARAMP ramp {};
            static_assert(sizeof(ramp) == sizeof(c.ramp), "D3DGAMMARAMP is 768 WORDs");
            std::memcpy(&ramp, c.ramp.data(), sizeof(ramp));
            d->SetGammaRamp(c.swapChain, c.flags, &ramp);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateTexture& c) {
        if (auto* d = dev()) {
            IDirect3DTexture9* t = nullptr;
            check(d->CreateTexture(c.width, c.height, c.levels, c.usage, static_cast<D3DFORMAT>(c.format),
                                   static_cast<D3DPOOL>(c.pool), &t, nullptr));
            link(c.result, t, Kind::Texture);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateVolumeTexture& c) {
        if (auto* d = dev()) {
            IDirect3DVolumeTexture9* t = nullptr;
            check(d->CreateVolumeTexture(c.width, c.height, c.depth, c.levels, c.usage, static_cast<D3DFORMAT>(c.format),
                                         static_cast<D3DPOOL>(c.pool), &t, nullptr));
            link(c.result, t, Kind::VolumeTexture);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateCubeTexture& c) {
        if (auto* d = dev()) {
            IDirect3DCubeTexture9* t = nullptr;
            check(d->CreateCubeTexture(c.edgeLength, c.levels, c.usage, static_cast<D3DFORMAT>(c.format),
                                       static_cast<D3DPOOL>(c.pool), &t, nullptr));
            link(c.result, t, Kind::CubeTexture);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateVertexBuffer& c) {
        if (auto* d = dev()) {
            IDirect3DVertexBuffer9* b = nullptr;
            check(d->CreateVertexBuffer(c.length, c.usage, c.fvf, static_cast<D3DPOOL>(c.pool), &b, nullptr));
            link(c.result, b, Kind::VertexBuffer);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateIndexBuffer& c) {
        if (auto* d = dev()) {
            IDirect3DIndexBuffer9* b = nullptr;
            check(d->CreateIndexBuffer(c.length, c.usage, static_cast<D3DFORMAT>(c.format), static_cast<D3DPOOL>(c.pool),
                                       &b, nullptr));
            link(c.result, b, Kind::IndexBuffer);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateRenderTarget& c) {
        if (auto* d = dev()) {
            IDirect3DSurface9* s = nullptr;
            check(d->CreateRenderTarget(c.width, c.height, static_cast<D3DFORMAT>(c.format),
                                        static_cast<D3DMULTISAMPLE_TYPE>(c.multiSample), c.multisampleQuality,
                                        static_cast<BOOL>(c.lockable), &s, nullptr));
            link(c.result, s, Kind::Surface);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateDepthStencilSurface& c) {
        if (auto* d = dev()) {
            IDirect3DSurface9* s = nullptr;
            check(d->CreateDepthStencilSurface(c.width, c.height, static_cast<D3DFORMAT>(c.format),
                                               static_cast<D3DMULTISAMPLE_TYPE>(c.multiSample), c.multisampleQuality,
                                               static_cast<BOOL>(c.discard), &s, nullptr));
            link(c.result, s, Kind::Surface);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_UpdateSurface& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* src = as<IDirect3DSurface9>(c.source, Kind::Surface);
            auto* dst = as<IDirect3DSurface9>(c.destination, Kind::Surface);
            RECT r {};
            POINT pt {};
            const RECT* pr = optionalWords(c.sourceRect, r, ok);
            const POINT* pp = optionalWords(c.destPoint, pt, ok);
            if (!ok) {
                bad = true;
                return;
            }
            if (src != nullptr && dst != nullptr) {
                check(d->UpdateSurface(src, pr, dst, pp));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_UpdateTexture& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* src = baseTexture(c.source, ok);
            auto* dst = baseTexture(c.destination, ok);
            if (ok) {
                check(d->UpdateTexture(src, dst));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_GetRenderTargetData& c) {
        if (auto* d = dev()) {
            auto* rt = as<IDirect3DSurface9>(c.renderTarget, Kind::Surface);
            auto* dst = as<IDirect3DSurface9>(c.destination, Kind::Surface);
            if (rt != nullptr && dst != nullptr) {
                check(d->GetRenderTargetData(rt, dst));
                readSurface(dst, nullptr);
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_GetFrontBufferData& c) {
        if (auto* d = dev()) {
            if (auto* dst = as<IDirect3DSurface9>(c.destination, Kind::Surface)) {
                check(d->GetFrontBufferData(c.swapChain, dst));
                readSurface(dst, nullptr);
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_StretchRect& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* src = as<IDirect3DSurface9>(c.source, Kind::Surface);
            auto* dst = as<IDirect3DSurface9>(c.destination, Kind::Surface);
            RECT sr {}, dr {};
            const RECT* psr = optionalWords(c.sourceRect, sr, ok);
            const RECT* pdr = optionalWords(c.destRect, dr, ok);
            if (!ok) {
                bad = true;
                return;
            }
            if (src != nullptr && dst != nullptr) {
                check(d->StretchRect(src, psr, dst, pdr, static_cast<D3DTEXTUREFILTERTYPE>(c.filter)));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_ColorFill& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* s = as<IDirect3DSurface9>(c.surface, Kind::Surface);
            RECT r {};
            const RECT* pr = optionalWords(c.rect, r, ok);
            if (!ok) {
                bad = true;
                return;
            }
            if (s != nullptr) {
                check(d->ColorFill(s, pr, c.color));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateOffscreenPlainSurface& c) {
        if (auto* d = dev()) {
            IDirect3DSurface9* s = nullptr;
            check(d->CreateOffscreenPlainSurface(c.width, c.height, static_cast<D3DFORMAT>(c.format),
                                                 static_cast<D3DPOOL>(c.pool), &s, nullptr));
            link(c.result, s, Kind::Surface);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetRenderTarget& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* s = opt<IDirect3DSurface9>(c.surface, Kind::Surface, ok);
            if (ok) {
                check(d->SetRenderTarget(c.index, s));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetDepthStencilSurface& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* s = opt<IDirect3DSurface9>(c.surface, Kind::Surface, ok);
            if (ok) {
                check(d->SetDepthStencilSurface(s));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_BeginScene&) {
        if (auto* d = dev()) {
            check(d->BeginScene());
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_EndScene&) {
        if (auto* d = dev()) {
            check(d->EndScene());
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_Clear& c) {
        if (auto* d = dev()) {
            if (c.rects.size() % 4 != 0) {
                bad = true;
                return;
            }
            const DWORD count = static_cast<DWORD>(c.rects.size() / 4);
            static_assert(sizeof(D3DRECT) == 16, "D3DRECT is 4 LONG");
            check(d->Clear(count, count != 0 ? reinterpret_cast<const D3DRECT*>(c.rects.data()) : nullptr, c.flags,
                           c.color, c.z, c.stencil));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetTransform& c) {
        if (auto* d = dev()) {
            D3DMATRIX m;
            std::memcpy(&m, c.matrix.data(), sizeof(m));
            check(d->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(c.state), &m));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_MultiplyTransform& c) {
        if (auto* d = dev()) {
            D3DMATRIX m;
            std::memcpy(&m, c.matrix.data(), sizeof(m));
            check(d->MultiplyTransform(static_cast<D3DTRANSFORMSTATETYPE>(c.state), &m));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetViewport& c) {
        if (auto* d = dev()) {
            D3DVIEWPORT9 vp {c.x, c.y, c.width, c.height, c.minZ, c.maxZ};
            check(d->SetViewport(&vp));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetMaterial& c) {
        if (auto* d = dev()) {
            D3DMATERIAL9 m;
            static_assert(sizeof(m) == sizeof(c.material), "D3DMATERIAL9 is 17 floats");
            std::memcpy(&m, c.material.data(), sizeof(m));
            check(d->SetMaterial(&m));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetLight& c) {
        if (auto* d = dev()) {
            D3DLIGHT9 l {};
            static_assert(sizeof(l) == 4 + sizeof(c.parameters), "D3DLIGHT9 is Type + 25 floats");
            l.Type = static_cast<D3DLIGHTTYPE>(c.type);
            std::memcpy(reinterpret_cast<uint8_t*>(&l) + 4, c.parameters.data(), sizeof(c.parameters));
            check(d->SetLight(c.index, &l));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_LightEnable& c) {
        if (auto* d = dev()) {
            check(d->LightEnable(c.index, static_cast<BOOL>(c.enable)));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetClipPlane& c) {
        if (auto* d = dev()) {
            check(d->SetClipPlane(c.index, c.plane.data()));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetRenderState& c) {
        if (auto* d = dev()) {
            check(d->SetRenderState(static_cast<D3DRENDERSTATETYPE>(c.state), c.value));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateStateBlock& c) {
        if (auto* d = dev()) {
            IDirect3DStateBlock9* sb = nullptr;
            check(d->CreateStateBlock(static_cast<D3DSTATEBLOCKTYPE>(c.type), &sb));
            link(c.result, sb, Kind::StateBlock);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_BeginStateBlock&) {
        if (auto* d = dev()) {
            check(d->BeginStateBlock());
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_EndStateBlock& c) {
        if (auto* d = dev()) {
            IDirect3DStateBlock9* sb = nullptr;
            check(d->EndStateBlock(&sb));
            link(c.result, sb, Kind::StateBlock);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetClipStatus& c) {
        if (auto* d = dev()) {
            D3DCLIPSTATUS9 cs {c.clipUnion, c.clipIntersection};
            check(d->SetClipStatus(&cs));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetTexture& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* t = baseTexture(c.texture, ok);
            if (ok) {
                check(d->SetTexture(c.stage, t));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetTextureStageState& c) {
        if (auto* d = dev()) {
            check(d->SetTextureStageState(c.stage, static_cast<D3DTEXTURESTAGESTATETYPE>(c.type), c.value));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetSamplerState& c) {
        if (auto* d = dev()) {
            check(d->SetSamplerState(c.sampler, static_cast<D3DSAMPLERSTATETYPE>(c.type), c.value));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_ValidateDevice&) {
        DWORD passes = 0;
        if (auto* d = dev()) {
            hr = d->ValidateDevice(&passes);
        }
        replyValue(passes);
    }
    void on(const cmd::IDirect3DDevice9Ex_SetPaletteEntries& c) {
        if (auto* d = dev()) {
            static_assert(sizeof(PALETTEENTRY) * 256 == sizeof(c.entries), "256 PALETTEENTRY");
            check(d->SetPaletteEntries(c.paletteNumber, reinterpret_cast<const PALETTEENTRY*>(c.entries.data())));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetCurrentTexturePalette& c) {
        if (auto* d = dev()) {
            check(d->SetCurrentTexturePalette(c.paletteNumber));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetScissorRect& c) {
        if (auto* d = dev()) {
            RECT r {c.rect[0], c.rect[1], c.rect[2], c.rect[3]};
            check(d->SetScissorRect(&r));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetSoftwareVertexProcessing& c) {
        if (auto* d = dev()) {
            check(d->SetSoftwareVertexProcessing(static_cast<BOOL>(c.software)));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetNPatchMode& c) {
        if (auto* d = dev()) {
            check(d->SetNPatchMode(c.segments));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_DrawPrimitive& c) {
        if (auto* d = dev()) {
            check(d->DrawPrimitive(static_cast<D3DPRIMITIVETYPE>(c.primitiveType), c.startVertex, c.primitiveCount));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_DrawIndexedPrimitive& c) {
        if (auto* d = dev()) {
            check(d->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(c.primitiveType), c.baseVertexIndex,
                                          c.minVertexIndex, c.numVertices, c.startIndex, c.primitiveCount));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_DrawPrimitiveUP& c) {
        if (auto* d = dev()) {
            if (c.vertexData.empty()) {
                bad = true;
                return;
            }
            check(d->DrawPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(c.primitiveType), c.primitiveCount, c.vertexData.data(),
                                     c.vertexStreamZeroStride));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_DrawPrimitiveUPHeap& c) {
        if (auto* d = dev()) {
            UploadView v;
            if (!resolveUpload(heap, c.heapChunk, c.heapBytes, {}, v) || v.size == 0) {
                bad = true;
                return;
            }
            check(d->DrawPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(c.primitiveType), c.primitiveCount, v.data,
                                     c.vertexStreamZeroStride));
            releaseUpload(heap, v);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_DrawIndexedPrimitiveUP& c) {
        if (auto* d = dev()) {
            Upload up(*this, c);
            if (!up.ok || c.indexBytes == 0 || c.indexBytes >= up.v.size) {
                bad = true;
                return;
            }
            check(d->DrawIndexedPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(c.primitiveType), c.minVertexIndex, c.numVertices,
                                            c.primitiveCount, up.v.data, static_cast<D3DFORMAT>(c.indexDataFormat),
                                            up.v.data + c.indexBytes, c.vertexStreamZeroStride));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_ProcessVertices& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* dst = as<IDirect3DVertexBuffer9>(c.destBuffer, Kind::VertexBuffer);
            auto* decl = opt<IDirect3DVertexDeclaration9>(c.vertexDecl, Kind::VertexDeclaration, ok);
            if (dst != nullptr && ok) {
                check(d->ProcessVertices(c.srcStartIndex, c.destIndex, c.vertexCount, dst, decl, c.flags));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateVertexDeclaration& c) {
        if (auto* d = dev()) {
            static_assert(sizeof(D3DVERTEXELEMENT9) == 8, "D3DVERTEXELEMENT9 is 8 bytes");
            const size_t n = c.elements.size() / 2;
            if (n == 0 || c.elements.size() % 2 != 0) {
                bad = true;
                return;
            }
            std::vector<D3DVERTEXELEMENT9> e(n);
            std::memcpy(e.data(), c.elements.data(), n * sizeof(D3DVERTEXELEMENT9));
            if (e.back().Stream != 0xFF) {
                bad = true;  // missing D3DDECL_END
                return;
            }
            IDirect3DVertexDeclaration9* decl = nullptr;
            check(d->CreateVertexDeclaration(e.data(), &decl));
            link(c.result, decl, Kind::VertexDeclaration);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetVertexDeclaration& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* decl = opt<IDirect3DVertexDeclaration9>(c.declaration, Kind::VertexDeclaration, ok);
            if (ok) {
                check(d->SetVertexDeclaration(decl));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetFVF& c) {
        if (auto* d = dev()) {
            check(d->SetFVF(c.fvf));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateVertexShader& c) {
        if (auto* d = dev()) {
            if (c.function.empty()) {
                bad = true;
                return;
            }
            IDirect3DVertexShader9* s = nullptr;
            check(d->CreateVertexShader(reinterpret_cast<const DWORD*>(c.function.data()), &s));
            link(c.result, s, Kind::VertexShader);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreatePixelShader& c) {
        if (auto* d = dev()) {
            if (c.function.empty()) {
                bad = true;
                return;
            }
            IDirect3DPixelShader9* s = nullptr;
            check(d->CreatePixelShader(reinterpret_cast<const DWORD*>(c.function.data()), &s));
            link(c.result, s, Kind::PixelShader);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetVertexShader& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* s = opt<IDirect3DVertexShader9>(c.shader, Kind::VertexShader, ok);
            if (ok) {
                check(d->SetVertexShader(s));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetPixelShader& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* s = opt<IDirect3DPixelShader9>(c.shader, Kind::PixelShader, ok);
            if (ok) {
                check(d->SetPixelShader(s));
            }
        }
    }
    // Shader constants: F and I carry 4 components per register, B one BOOL per register.
    void on(const cmd::IDirect3DDevice9Ex_SetVertexShaderConstantF& c) {
        if (auto* d = dev()) {
            check(d->SetVertexShaderConstantF(c.startRegister, c.data.data(), static_cast<UINT>(c.data.size() / 4)));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetVertexShaderConstantI& c) {
        if (auto* d = dev()) {
            check(d->SetVertexShaderConstantI(c.startRegister, reinterpret_cast<const int*>(c.data.data()),
                                              static_cast<UINT>(c.data.size() / 4)));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetVertexShaderConstantB& c) {
        if (auto* d = dev()) {
            check(d->SetVertexShaderConstantB(c.startRegister, reinterpret_cast<const BOOL*>(c.data.data()),
                                              static_cast<UINT>(c.data.size())));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetPixelShaderConstantF& c) {
        if (auto* d = dev()) {
            check(d->SetPixelShaderConstantF(c.startRegister, c.data.data(), static_cast<UINT>(c.data.size() / 4)));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetPixelShaderConstantI& c) {
        if (auto* d = dev()) {
            check(d->SetPixelShaderConstantI(c.startRegister, reinterpret_cast<const int*>(c.data.data()),
                                             static_cast<UINT>(c.data.size() / 4)));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetPixelShaderConstantB& c) {
        if (auto* d = dev()) {
            check(d->SetPixelShaderConstantB(c.startRegister, reinterpret_cast<const BOOL*>(c.data.data()),
                                             static_cast<UINT>(c.data.size())));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetStreamSource& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* vb = opt<IDirect3DVertexBuffer9>(c.vertexBuffer, Kind::VertexBuffer, ok);
            if (ok) {
                check(d->SetStreamSource(c.streamNumber, vb, c.offsetInBytes, c.stride));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetStreamSourceFreq& c) {
        if (auto* d = dev()) {
            check(d->SetStreamSourceFreq(c.streamNumber, c.setting));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetIndices& c) {
        if (auto* d = dev()) {
            bool ok = true;
            auto* ib = opt<IDirect3DIndexBuffer9>(c.indexBuffer, Kind::IndexBuffer, ok);
            if (ok) {
                check(d->SetIndices(ib));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_DrawRectPatch& c) {
        if (auto* d = dev()) {
            bool ok = true;
            float segs[4] = {};
            D3DRECTPATCH_INFO info {};
            const D3DRECTPATCH_INFO* pi = optionalWords(c.rectPatchInfo, info, ok);
            if (!ok || (!c.numSegs.empty() && c.numSegs.size() != 4)) {
                bad = true;
                return;
            }
            std::copy(c.numSegs.begin(), c.numSegs.end(), segs);
            check(d->DrawRectPatch(c.patchHandle, c.numSegs.empty() ? nullptr : segs, pi));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_DrawTriPatch& c) {
        if (auto* d = dev()) {
            bool ok = true;
            float segs[3] = {};
            D3DTRIPATCH_INFO info {};
            const D3DTRIPATCH_INFO* pi = optionalWords(c.triPatchInfo, info, ok);
            if (!ok || (!c.numSegs.empty() && c.numSegs.size() != 3)) {
                bad = true;
                return;
            }
            std::copy(c.numSegs.begin(), c.numSegs.end(), segs);
            check(d->DrawTriPatch(c.patchHandle, c.numSegs.empty() ? nullptr : segs, pi));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_DeletePatch& c) {
        if (auto* d = dev()) {
            check(d->DeletePatch(c.patchHandle));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateQuery& c) {
        if (auto* d = dev()) {
            IDirect3DQuery9* q = nullptr;
            check(d->CreateQuery(static_cast<D3DQUERYTYPE>(c.type), c.result != 0 ? &q : nullptr));
            link(c.result, q, Kind::Query);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetConvolutionMonoKernel& c) {
        if (auto* d = devEx()) {
            check(d->SetConvolutionMonoKernel(c.width, c.height, const_cast<float*>(c.rows.data()),
                                              const_cast<float*>(c.columns.data())));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_ComposeRects& c) {
        if (auto* d = devEx()) {
            auto* src = as<IDirect3DSurface9>(c.source, Kind::Surface);
            auto* dst = as<IDirect3DSurface9>(c.destination, Kind::Surface);
            auto* srcDescs = as<IDirect3DVertexBuffer9>(c.srcRectDescs, Kind::VertexBuffer);
            auto* dstDescs = as<IDirect3DVertexBuffer9>(c.dstRectDescs, Kind::VertexBuffer);
            if (src != nullptr && dst != nullptr && srcDescs != nullptr && dstDescs != nullptr) {
                check(d->ComposeRects(src, dst, srcDescs, c.numRects, dstDescs, static_cast<D3DCOMPOSERECTSOP>(c.operation),
                                      c.xOffset, c.yOffset));
            }
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_PresentEx& c) {
        if (auto* d = devEx()) {
            bool ok = true;
            RECT src {}, dst {};
            const RECT* ps = optionalWords(c.sourceRect, src, ok);
            const RECT* pd = optionalWords(c.destRect, dst, ok);
            if (!ok) {
                bad = true;
                return;
            }
            check(d->PresentEx(ps, pd, mapWindow(toHwnd(c.destWindowOverride)), nullptr, c.flags));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetGPUThreadPriority& c) {
        if (auto* d = devEx()) {
            check(d->SetGPUThreadPriority(c.priority));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_WaitForVBlank& c) {
        if (auto* d = devEx()) {
            check(d->WaitForVBlank(c.swapChain));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CheckResourceResidency& c) {
        if (auto* d = devEx()) {
            std::vector<IDirect3DResource9*> res;
            for (uint32_t h : c.resources) {
                handle = h;
                IDirect3DResource9* r = resource();
                if (r == nullptr) {
                    return;
                }
                res.push_back(r);
            }
            hr = d->CheckResourceResidency(res.data(), static_cast<UINT32>(res.size()));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_SetMaximumFrameLatency& c) {
        if (auto* d = devEx()) {
            check(d->SetMaximumFrameLatency(c.maxLatency));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CheckDeviceState& c) {
        if (auto* d = devEx()) {
            hr = d->CheckDeviceState(mapWindow(toHwnd(c.destinationWindow)));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateRenderTargetEx& c) {
        if (auto* d = devEx()) {
            IDirect3DSurface9* s = nullptr;
            check(d->CreateRenderTargetEx(c.width, c.height, static_cast<D3DFORMAT>(c.format),
                                          static_cast<D3DMULTISAMPLE_TYPE>(c.multiSample), c.multisampleQuality,
                                          static_cast<BOOL>(c.lockable), &s, nullptr, c.usage));
            link(c.result, s, Kind::Surface);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateOffscreenPlainSurfaceEx& c) {
        if (auto* d = devEx()) {
            IDirect3DSurface9* s = nullptr;
            check(d->CreateOffscreenPlainSurfaceEx(c.width, c.height, static_cast<D3DFORMAT>(c.format),
                                                   static_cast<D3DPOOL>(c.pool), &s, nullptr, c.usage));
            link(c.result, s, Kind::Surface);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx& c) {
        if (auto* d = devEx()) {
            IDirect3DSurface9* s = nullptr;
            check(d->CreateDepthStencilSurfaceEx(c.width, c.height, static_cast<D3DFORMAT>(c.format),
                                                 static_cast<D3DMULTISAMPLE_TYPE>(c.multiSample), c.multisampleQuality,
                                                 static_cast<BOOL>(c.discard), &s, nullptr, c.usage));
            link(c.result, s, Kind::Surface);
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_ResetEx& c) {
        if (auto* d = devEx()) {
            bool ok = true;
            D3DPRESENT_PARAMETERS pp = toPresentParameters(c.presentParameters);
            pp.hDeviceWindow = mapWindow(pp.hDeviceWindow);
            D3DDISPLAYMODEEX mode {};
            const D3DDISPLAYMODEEX* pm = optionalWords(c.fullscreenDisplayMode, mode, ok);
            if (!ok) {
                bad = true;
                return;
            }
            check(d->ResetEx(&pp, const_cast<D3DDISPLAYMODEEX*>(pm)));
        }
    }
    void on(const cmd::IDirect3DDevice9Ex_GetDisplayModeEx& c) {
        D3DDISPLAYMODEEX m {};
        m.Size = sizeof(m);
        D3DDISPLAYROTATION rot = D3DDISPLAYROTATION_IDENTITY;
        if (auto* d = devEx()) {
            check(d->GetDisplayModeEx(c.swapChain, &m, &rot));
        }
        replyModeEx(m, static_cast<uint32_t>(rot));
    }

    // ---- IDirect3DStateBlock9 / IDirect3DSwapChain9 ----------------------------------------------
    void on(const cmd::IDirect3DStateBlock9_Destroy&) { destroy(); }
    void on(const cmd::IDirect3DStateBlock9_Capture&) {
        if (auto* sb = as<IDirect3DStateBlock9>(handle, Kind::StateBlock)) {
            check(sb->Capture());
        }
    }
    void on(const cmd::IDirect3DStateBlock9_Apply&) {
        if (auto* sb = as<IDirect3DStateBlock9>(handle, Kind::StateBlock)) {
            check(sb->Apply());
        }
    }
    void on(const cmd::IDirect3DSwapChain9_Destroy&) { destroy(); }
    void on(const cmd::IDirect3DSwapChain9_Present& c) {
        if (auto* sc = as<IDirect3DSwapChain9>(handle, Kind::SwapChain)) {
            bool ok = true;
            RECT src {}, dst {};
            const RECT* ps = optionalWords(c.sourceRect, src, ok);
            const RECT* pd = optionalWords(c.destRect, dst, ok);
            if (!ok) {
                bad = true;
                return;
            }
            check(sc->Present(ps, pd, mapWindow(toHwnd(c.destWindowOverride)), nullptr, c.flags));
        }
    }
    void on(const cmd::IDirect3DSwapChain9_GetFrontBufferData& c) {
        if (auto* sc = as<IDirect3DSwapChain9>(handle, Kind::SwapChain)) {
            if (auto* dst = as<IDirect3DSurface9>(c.destination, Kind::Surface)) {
                check(sc->GetFrontBufferData(dst));
                readSurface(dst, nullptr);
            }
        }
    }
    void on(const cmd::IDirect3DSwapChain9_GetBackBuffer& c) {
        if (auto* sc = as<IDirect3DSwapChain9>(handle, Kind::SwapChain)) {
            IDirect3DSurface9* s = nullptr;
            check(sc->GetBackBuffer(c.backBuffer, static_cast<D3DBACKBUFFER_TYPE>(c.type), &s));
            link(c.result, s, Kind::Surface);
        }
    }
    void on(const cmd::IDirect3DSwapChain9_GetRasterStatus&) {
        D3DRASTER_STATUS rs {};
        if (auto* sc = as<IDirect3DSwapChain9>(handle, Kind::SwapChain)) {
            check(sc->GetRasterStatus(&rs));
        }
        replyRaster(rs);
    }
    void on(const cmd::IDirect3DSwapChain9_GetDisplayMode&) {
        D3DDISPLAYMODE m {};
        if (auto* sc = as<IDirect3DSwapChain9>(handle, Kind::SwapChain)) {
            check(sc->GetDisplayMode(&m));
        }
        replyMode(m);
    }
    IDirect3DSwapChain9Ex* swapChainEx() {
        auto* sc = as<IDirect3DSwapChain9>(handle, Kind::SwapChain);
        IDirect3DSwapChain9Ex* ex = nullptr;
        if (sc == nullptr || FAILED(sc->QueryInterface(__uuidof(IDirect3DSwapChain9Ex), reinterpret_cast<void**>(&ex)))) {
            hr = D3DERR_INVALIDCALL;
            return nullptr;
        }
        ex->Release();
        return ex;
    }
    void on(const cmd::IDirect3DSwapChain9_GetLastPresentCount&) {
        UINT n = 0;
        if (auto* sc = swapChainEx()) {
            check(sc->GetLastPresentCount(&n));
        }
        replyValue(n);
    }
    void on(const cmd::IDirect3DSwapChain9_GetDisplayModeEx&) {
        D3DDISPLAYMODEEX m {};
        m.Size = sizeof(m);
        D3DDISPLAYROTATION rot = D3DDISPLAYROTATION_IDENTITY;
        if (auto* sc = swapChainEx()) {
            check(sc->GetDisplayModeEx(&m, &rot));
        }
        replyModeEx(m, static_cast<uint32_t>(rot));
    }

    // ---- resources (the per-interface copies are folded into these two, RL-2.2) -------------------
    void on(const cmd::IDirect3DResource9_Destroy&) { destroy(); }
    void on(const cmd::IDirect3DResource9_SetPriority& c) {
        if (auto* r = resource()) {
            r->SetPriority(c.priority);
        }
    }
    void on(const cmd::IDirect3DResource9_PreLoad&) {
        if (auto* r = resource()) {
            r->PreLoad();
        }
    }
    void on(const cmd::IDirect3DBaseTexture9_SetLOD& c) {
        if (auto* t = baseTexture()) {
            t->SetLOD(c.lod);
        }
    }
    void on(const cmd::IDirect3DBaseTexture9_SetAutoGenFilterType& c) {
        if (auto* t = baseTexture()) {
            check(t->SetAutoGenFilterType(static_cast<D3DTEXTUREFILTERTYPE>(c.filterType)));
        }
    }
    void on(const cmd::IDirect3DBaseTexture9_GenerateMipSubLevels&) {
        if (auto* t = baseTexture()) {
            t->GenerateMipSubLevels();
        }
    }
    void on(const cmd::IDirect3DVertexDeclaration9_Destroy&) { destroy(); }
    void on(const cmd::IDirect3DVertexShader9_Destroy&) { destroy(); }
    void on(const cmd::IDirect3DPixelShader9_Destroy&) { destroy(); }

    // Textures: level links, lock read-backs, unlock uploads.
    IDirect3DSurface9* textureLevel(uint32_t level) {
        auto* t = as<IDirect3DTexture9>(handle, Kind::Texture);
        IDirect3DSurface9* s = nullptr;
        if (t != nullptr) {
            check(t->GetSurfaceLevel(level, &s));
        }
        return s;
    }
    IDirect3DSurface9* cubeFace(uint32_t face, uint32_t level) {
        auto* t = as<IDirect3DCubeTexture9>(handle, Kind::CubeTexture);
        IDirect3DSurface9* s = nullptr;
        if (t != nullptr) {
            check(t->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face), level, &s));
        }
        return s;
    }
    void readRect(IDirect3DSurface9* s, const std::vector<int32_t>& rect) {
        if (s == nullptr) {
            return;
        }
        bool ok = true;
        RECT r {};
        const RECT* pr = optionalWords(rect, r, ok);
        if (!ok) {
            bad = true;
        } else {
            readSurface(s, pr);
        }
        s->Release();
    }
    void on(const cmd::IDirect3DTexture9_GetSurfaceLevel& c) {
        if (auto* t = as<IDirect3DTexture9>(handle, Kind::Texture)) {
            IDirect3DSurface9* s = nullptr;
            check(t->GetSurfaceLevel(c.level, &s));
            link(c.result, s, Kind::Surface);
        }
    }
    void on(const cmd::IDirect3DTexture9_LockRect& c) { readRect(textureLevel(c.level), c.rect); }
    void on(const cmd::IDirect3DTexture9_UnlockRect& c) {
        if (IDirect3DSurface9* s = textureLevel(c.level)) {
            unlockSurface(s, c);
            s->Release();
        }
    }
    void on(const cmd::IDirect3DTexture9_AddDirtyRect& c) {
        if (auto* t = as<IDirect3DTexture9>(handle, Kind::Texture)) {
            bool ok = true;
            RECT r {};
            const RECT* pr = optionalWords(c.rect, r, ok);
            if (ok) {
                check(t->AddDirtyRect(pr));
            }
        }
    }
    void on(const cmd::IDirect3DVolumeTexture9_GetVolumeLevel& c) {
        if (auto* t = as<IDirect3DVolumeTexture9>(handle, Kind::VolumeTexture)) {
            IDirect3DVolume9* v = nullptr;
            check(t->GetVolumeLevel(c.level, &v));
            link(c.result, v, Kind::Volume);
        }
    }
    void on(const cmd::IDirect3DVolumeTexture9_LockBox& c) {
        if (auto* t = as<IDirect3DVolumeTexture9>(handle, Kind::VolumeTexture)) {
            bool ok = true;
            D3DBOX b {};
            const D3DBOX* pb = optionalWords(c.box, b, ok);
            if (!ok) {
                bad = true;
                return;
            }
            readVolume(t, pb, c.level);
        }
    }
    void on(const cmd::IDirect3DVolumeTexture9_UnlockBox& c) {
        unlockVolume(as<IDirect3DVolumeTexture9>(handle, Kind::VolumeTexture), c.level, c);
    }
    void on(const cmd::IDirect3DVolumeTexture9_AddDirtyBox& c) {
        if (auto* t = as<IDirect3DVolumeTexture9>(handle, Kind::VolumeTexture)) {
            bool ok = true;
            D3DBOX b {};
            const D3DBOX* pb = optionalWords(c.box, b, ok);
            if (ok) {
                check(t->AddDirtyBox(pb));
            }
        }
    }
    void on(const cmd::IDirect3DCubeTexture9_GetCubeMapSurface& c) {
        if (auto* t = as<IDirect3DCubeTexture9>(handle, Kind::CubeTexture)) {
            IDirect3DSurface9* s = nullptr;
            check(t->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(c.face), c.level, &s));
            link(c.result, s, Kind::Surface);
        }
    }
    void on(const cmd::IDirect3DCubeTexture9_LockRect& c) { readRect(cubeFace(c.face, c.level), c.rect); }
    void on(const cmd::IDirect3DCubeTexture9_UnlockRect& c) {
        if (IDirect3DSurface9* s = cubeFace(c.face, c.level)) {
            unlockSurface(s, c);
            s->Release();
        }
    }
    void on(const cmd::IDirect3DCubeTexture9_AddDirtyRect& c) {
        if (auto* t = as<IDirect3DCubeTexture9>(handle, Kind::CubeTexture)) {
            bool ok = true;
            RECT r {};
            const RECT* pr = optionalWords(c.rect, r, ok);
            if (ok) {
                check(t->AddDirtyRect(static_cast<D3DCUBEMAP_FACES>(c.face), pr));
            }
        }
    }
    // Buffers: Lock is a read-back (the client only sends it when it must read host data).
    void on(const cmd::IDirect3DVertexBuffer9_Lock& c) {
        readBuffer(as<IDirect3DVertexBuffer9>(handle, Kind::VertexBuffer), c.offset, c.size);
    }
    void on(const cmd::IDirect3DVertexBuffer9_Unlock& c) {
        unlockBuffer(as<IDirect3DVertexBuffer9>(handle, Kind::VertexBuffer), c);
    }
    void on(const cmd::IDirect3DIndexBuffer9_Lock& c) {
        readBuffer(as<IDirect3DIndexBuffer9>(handle, Kind::IndexBuffer), c.offset, c.size);
    }
    void on(const cmd::IDirect3DIndexBuffer9_Unlock& c) {
        unlockBuffer(as<IDirect3DIndexBuffer9>(handle, Kind::IndexBuffer), c);
    }
    // Surfaces, volumes.
    void on(const cmd::IDirect3DSurface9_LockRect& c) {
        if (auto* s = as<IDirect3DSurface9>(handle, Kind::Surface)) {
            s->AddRef();
            readRect(s, c.rect);
        }
    }
    void on(const cmd::IDirect3DSurface9_UnlockRect& c) { unlockSurface(as<IDirect3DSurface9>(handle, Kind::Surface), c); }
    void on(const cmd::IDirect3DVolume9_LockBox& c) {
        if (auto* v = as<IDirect3DVolume9>(handle, Kind::Volume)) {
            bool ok = true;
            D3DBOX b {};
            const D3DBOX* pb = optionalWords(c.box, b, ok);
            if (!ok) {
                bad = true;
                return;
            }
            readVolume(v, pb, 0);
        }
    }
    void on(const cmd::IDirect3DVolume9_UnlockBox& c) { unlockVolume(as<IDirect3DVolume9>(handle, Kind::Volume), 0, c); }
    // IDirect3DQuery9
    void on(const cmd::IDirect3DQuery9_Destroy&) { destroy(); }
    void on(const cmd::IDirect3DQuery9_Issue& c) {
        if (auto* q = as<IDirect3DQuery9>(handle, Kind::Query)) {
            check(q->Issue(c.issueFlags));
        }
    }
    void on(const cmd::IDirect3DQuery9_GetData& c) {
        if (auto* q = as<IDirect3DQuery9>(handle, Kind::Query)) {
            if (c.size > 4096) {
                bad = true;
                return;
            }
            std::vector<uint8_t> data(c.size);
            hr = q->GetData(c.size != 0 ? data.data() : nullptr, c.size, c.getDataFlags);
            if (hr == S_OK) {
                payload = std::move(data);
            }
        }
    }
};

D3D9Executor::D3D9Executor() : impl_(new Impl) {}

D3D9Executor::~D3D9Executor() { releaseAll(); }

void D3D9Executor::releaseAll() {
    Impl& im = *impl_;
    // Children first, devices last, then the IDirect3D9 object. The module stays loaded (upstream:
    // FreeLibrary on DXVK deadlocked on its worker threads).
    for (int pass = 0; pass < 2; ++pass) {
        for (auto& [h, o] : im.objs) {
            (void) h;
            if ((o.kind == Kind::Device) == (pass == 1)) {
                Impl::releaseObj(o);
            }
        }
    }
    im.objs.clear();
    for (auto& [h, o] : im.d3ds) {
        (void) h;
        Impl::releaseD3D(o);
    }
    im.d3ds.clear();
    Impl::releaseD3D(im.def);
    im.d3d = nullptr;
    im.d3dEx = nullptr;
    for (auto& [client, own] : im.standIns) {
        (void) client;
        ::DestroyWindow(own);
    }
    im.standIns.clear();
}

bool D3D9Executor::load(const D3D9ExecutorConfig& config, std::string& error) {
    Impl& im = *impl_;
    im.verbose = config.verbose;
    im.ownWindowFallback = config.ownWindowFallback;
    if (config.relight >= 0) {
        setProcessEnv("FUSE_RELIGHT", config.relight != 0 ? "1" : "0");
    }
    im.module = ::LoadLibraryExA(config.d3d9Path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (im.module == nullptr) {
        error = "cannot load " + config.d3d9Path + " (error " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    im.createExFn = reinterpret_cast<Impl::CreateExFn>(
        reinterpret_cast<void (*)(void)>(::GetProcAddress(im.module, "Direct3DCreate9Ex")));
    im.createFn = reinterpret_cast<Impl::CreateFn>(
        reinterpret_cast<void (*)(void)>(::GetProcAddress(im.module, "Direct3DCreate9")));
    if (im.createFn == nullptr) {
        error = config.d3d9Path + ": no Direct3DCreate9 export";
        return false;
    }
    // The interfaces are created by Direct3DCreate9 commands (plain or Ex, as the game asked).
    return true;
}

void D3D9Executor::setSharedHeap(ipc::SharedHeap* heap) { impl_->heap = heap; }

int32_t D3D9Executor::execute(uint16_t command, uint16_t flags, uint32_t handle, const uint8_t* data, size_t size,
                              Response* out) {
    (void) flags;
    Impl& im = *impl_;
    im.handle = handle;
    im.hr = S_OK;
    im.bad = false;
    im.payload.clear();
    ++im.stats.executed;
    const bool d3dCall = interfaceName(command) == "IDirect3D9Ex";
    if (d3dCall && !im.selectD3D(handle)) {
        im.hr = D3DERR_NOTAVAILABLE;
        if (out != nullptr) {
            out->result = static_cast<int32_t>(im.hr);
            out->payload.clear();
        }
        return static_cast<int32_t>(im.hr);
    }
    {
        const schema::DecodeStatus st = schema::dispatch(command, data, size, [&im](const auto& c) { im.on(c); });
        if (st != schema::DecodeStatus::Ok || im.bad) {
            ++im.stats.malformed;
            im.hr = D3DERR_INVALIDCALL;
            im.payload.clear();  // the host loop answers typed queries with a default reply
        }
    }
    if (im.verbose && FAILED(im.hr)) {
        std::fprintf(stderr, "fuse-relight executor: %s -> 0x%08lx\n", schema::commandName(command),
                     static_cast<unsigned long>(im.hr));
    }
    if (out != nullptr) {
        out->result = static_cast<int32_t>(im.hr);
        out->payload = std::move(im.payload);
        im.payload.clear();
    }
    return static_cast<int32_t>(im.hr);
}

bool D3D9Executor::terminated() const noexcept { return impl_->terminated; }
const D3D9ExecutorStats& D3D9Executor::stats() const noexcept { return impl_->stats; }
size_t D3D9Executor::liveObjects() const noexcept { return impl_->objs.size(); }

std::string thisModuleDirectory() {
    HMODULE self = nullptr;
    ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(&thisModuleDirectory), &self);
    char path[MAX_PATH * 4] = {};
    const DWORD n = ::GetModuleFileNameA(self, path, static_cast<DWORD>(sizeof(path)));
    std::string p(path, n);
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}

std::string passthroughD3D9Path() {
    char buf[MAX_PATH * 4] = {};
    const DWORD n = ::GetEnvironmentVariableA("FUSE_RELIGHT_PASSTHROUGH_D3D9", buf, static_cast<DWORD>(sizeof(buf)));
    if (n != 0 && n < sizeof(buf)) {
        return std::string(buf, n);
    }
    return thisModuleDirectory() + "fuse_relight_passthrough\\d3d9.dll";
}

BackendFactory makePassthroughFactory(const std::string& d3d9Path) {
    return [d3d9Path](std::string& error) -> std::unique_ptr<ILocalBackend> {
        auto exec = std::make_unique<D3D9Executor>();
        D3D9ExecutorConfig cfg;
        cfg.d3d9Path = d3d9Path.empty() ? passthroughD3D9Path() : d3d9Path;
        cfg.relight = 0;  // plain DXVK: no tap, no device import
        if (!exec->load(cfg, error)) {
            return nullptr;
        }
        return exec;
    };
}

}  // namespace fuse::relight::bridge::host
