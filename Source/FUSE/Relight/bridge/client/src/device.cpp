/*
 * Copyright (c) 2022-2025, NVIDIA CORPORATION. All rights reserved.
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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix bridge/src/client/{d3d9_device.cpp,d3d9_device_base.cpp}@0867d3c

// FUSE Relight RL-2.2: client IDirect3DDevice9Ex.
//
// Semantics kept from upstream d3d9_device.cpp: every Set* updates the client copy of the state
// and is sent without waiting; every Get* is answered from the copy; creation calls allocate the
// client object and its handle up front; the implicit swap chain, back buffer and auto depth-stencil
// are client objects linked to the host's implicit ones; Reset releases and relinks them;
// DrawPrimitiveUP / DrawIndexedPrimitiveUP ship the vertex (and index) data with the draw and then
// unbind stream 0 / the indices as D3D9 does; D3DDEVICE_CREATION_PARAMETERS, gamma, palettes, clip
// status, N-patch mode, frame latency and GPU priority are client state.
//
// Changes (revamp):
// - Create* wait for the host's HRESULT (upstream: only in its SendAllServerResponses debug mode),
//   so an unsupported format fails the call instead of leaving a dangling client object.
// - Present waits for the host (one round trip per frame, the frame pacing upstream got from its
//   Present semaphore) and returns the host's HRESULT; D3DERR_DEVICELOST once the host is gone.
// - GetRenderTargetData / GetFrontBufferData / StretchRect / ColorFill / UpdateSurface /
//   UpdateTexture / ProcessVertices mark their destinations host-dirty: the next lock reads the
//   region back (objects.hpp).
// - State-block recording goes to the block, not the device copy (device_state.hpp).
// - SetRenderTarget(0) resets the viewport and scissor rectangle like D3D9.
// - Shader bytecode length follows the token stream (comments and SM2+ instruction lengths), not
//   a scan for the first 0x0000FFFF word.
#include "device.hpp"

#include "window.hpp"

#include <algorithm>
#include <cstring>

namespace fuse::relight::bridge::client {

namespace cmd = schema::cmd;

namespace {

template <class I>
I* asIface(BridgeObject* o) {
    // The object's main interface derives from I along a single-inheritance COM chain, so its
    // IUnknown pointer is also its I pointer.
    return o ? reinterpret_cast<I*>(o->unknown()) : nullptr;
}

template <class I>
HRESULT returnObject(BridgeObject* o, I** out) {
    if (out == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *out = asIface<I>(o);
    if (o == nullptr) {
        return D3DERR_NOTFOUND;
    }
    o->addRef();
    return D3D_OK;
}

uint32_t vertexCount(D3DPRIMITIVETYPE type, UINT count) {
    switch (type) {
    case D3DPT_POINTLIST: return count;
    case D3DPT_LINELIST: return count * 2;
    case D3DPT_LINESTRIP: return count + 1;
    case D3DPT_TRIANGLELIST: return count * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return count + 2;
    default: return 0;
    }
}

// Length (in DWORDs, D3DSIO_END included) of a shader token stream; 0 if malformed.
// DrawPrimitiveUP vertex data sent inline (the RL-2.1 row has no heap fields); bigger draws take the
// indexed path. Well under half of the default 8 MiB client->host ring.
constexpr size_t kUpInlineMax = size_t(2) << 20;

size_t shaderLength(const DWORD* code) {
    constexpr size_t kMax = 1u << 20;
    const uint32_t major = (code[0] >> 8) & 0xFF;
    size_t i = 1;
    while (i < kMax) {
        const DWORD t = code[i];
        if (t == 0x0000FFFF) {
            return i + 1;
        }
        if ((t & 0xFFFF) == 0xFFFE) {  // comment
            i += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        if (major >= 2 && !(t & 0x80000000u)) {
            i += 1 + ((t >> 24) & 0xF);
            continue;
        }
        ++i;
    }
    return 0;
}

std::vector<uint32_t> toWords(const void* p, size_t dwords) {
    std::vector<uint32_t> v(dwords);
    std::memcpy(v.data(), p, dwords * 4);
    return v;
}

}  // namespace

// ---- lifetime -------------------------------------------------------------------------------------

Device::Device(uint32_t handle, Interface* parent, bool ex, const D3DDEVICE_CREATION_PARAMETERS& cp, const D3DCAPS9& caps)
    : BridgeObject(Kind::Device, handle, nullptr, nullptr), parent_(parent), ex_(ex), createParams_(cp), caps_(caps) {
    softwareVpConsts_ = (cp.BehaviorFlags & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING)) != 0;
    softwareVp_ = (cp.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0;
    state_.sizeConstants(softwareVpConsts_);
    for (uint32_t i = 0; i < 256; ++i) {
        gamma_.red[i] = gamma_.green[i] = gamma_.blue[i] = WORD(i * 257);
    }
    parent_->addRef();
}

HRESULT Device::query(REFIID riid, void** ppv) {
    if (ppv != nullptr && isDxvkDeviceBridgeIid(riid)) {
        FUSE_BRIDGE_LOCK();
        if (dxvkBridge_ == nullptr) {
            dxvkBridge_ = createDxvkDeviceBridge(this);
        }
        if (dxvkBridge_ != nullptr) {
            dxvkBridge_->AddRef();
            *ppv = dxvkBridge_;
            return S_OK;
        }
    }
    if (ppv != nullptr && !ex_ && riid == __uuidof(IDirect3DDevice9Ex)) {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    return queryCommon(this, static_cast<IDirect3DDevice9Ex*>(this), riid, ppv,
                       {&__uuidof(IDirect3DDevice9), &__uuidof(IDirect3DDevice9Ex)});
}

void Device::initImplicit(const D3DPRESENT_PARAMETERS& pp) {
    swapChain_ = new SwapChain(newHandle(), this, pp, 0);
    swapChain_->addRefPrivate();
    cmd::IDirect3DDevice9Ex_LinkSwapchain ls;
    ls.index = 0;
    ls.swapChain = swapChain_->handle();
    bridge().post(ls, handle());
    renderTargets_[0].reset(swapChain_->backBuffer(0));
    if (pp.EnableAutoDepthStencil) {
        D3DSURFACE_DESC d {};
        d.Format = pp.AutoDepthStencilFormat;
        d.Type = D3DRTYPE_SURFACE;
        d.Usage = D3DUSAGE_DEPTHSTENCIL;
        d.Pool = D3DPOOL_DEFAULT;
        d.MultiSampleType = pp.MultiSampleType;
        d.MultiSampleQuality = pp.MultiSampleQuality;
        d.Width = pp.BackBufferWidth;
        d.Height = pp.BackBufferHeight;
        autoDepth_ = new Surface(newHandle(), this, d, nullptr, nullptr);
        autoDepth_->addRefPrivate();
        cmd::IDirect3DDevice9Ex_LinkAutoDepthStencil la;
        la.surface = autoDepth_->handle();
        bridge().post(la, handle());
        depthStencil_.reset(autoDepth_);
    }
    resetDeviceState(state_, pp.EnableAutoDepthStencil != 0);
    state_.viewport = D3DVIEWPORT9 {0, 0, pp.BackBufferWidth, pp.BackBufferHeight, 0.0f, 1.0f};
    state_.scissorRect = RECT {0, 0, LONG(pp.BackBufferWidth), LONG(pp.BackBufferHeight)};
    windowAttach(pp.hDeviceWindow ? pp.hDeviceWindow : createParams_.hFocusWindow, pp, createParams_);
}

void Device::releaseImplicit() {
    for (auto& rt : renderTargets_) {
        rt.reset(nullptr);
    }
    depthStencil_.reset(nullptr);
    if (autoDepth_ != nullptr) {
        autoDepth_->releasePrivate();
        autoDepth_ = nullptr;
    }
    if (swapChain_ != nullptr) {
        swapChain_->releasePrivate();
        swapChain_ = nullptr;
    }
}

void Device::finalRelease() {
    if (recording_ != nullptr) {
        recording_->releasePrivate();
        recording_ = nullptr;
    }
    resetDeviceState(state_, false);  // drops every bound object
    releaseImplicit();
    windowDetach();
    if (dxvkBridge_ != nullptr) {
        destroyDxvkDeviceBridge(dxvkBridge_);
        dxvkBridge_ = nullptr;
    }
    destroyOnHost();
    Interface* parent = parent_;
    delete this;
    parent->release();
}

void Device::destroyOnHost() { bridge().post(cmd::IDirect3DDevice9Ex_Destroy {}, handle()); }

DeviceState& Device::target() { return recording_ ? recording_->state : state_; }

template <class Sb>
void Device::recordMask(Sb&& setMask) {
    if (recording_ != nullptr) {
        setMask(recording_->mask);
    }
}

void Device::markTargetDirty(BridgeObject* t) {
    if (t == nullptr) {
        return;
    }
    switch (t->kind()) {
    case Kind::Surface: static_cast<Surface*>(t)->markHostDirty(); break;
    case Kind::Volume: static_cast<Volume*>(t)->sub().hostDirty = true; break;
    case Kind::Texture: static_cast<Texture*>(t)->markHostDirty(); break;
    case Kind::CubeTexture: static_cast<CubeTexture*>(t)->markHostDirty(); break;
    case Kind::VolumeTexture: static_cast<VolumeTexture*>(t)->markHostDirty(); break;
    case Kind::VertexBuffer: static_cast<VertexBuffer*>(t)->markHostDirty(); break;
    case Kind::IndexBuffer: static_cast<IndexBuffer*>(t)->markHostDirty(); break;
    default: break;
    }
}

// ---- device queries ------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::TestCooperativeLevel() { return bridge().alive() ? D3D_OK : D3DERR_DEVICELOST; }

UINT STDMETHODCALLTYPE Device::GetAvailableTextureMem() {
    FUSE_BRIDGE_LOCK();
    cmd::Reply_Value r;
    return bridge().query(cmd::IDirect3DDevice9Ex_GetAvailableTextureMem {}, handle(), r) ? r.value : 0;
}

HRESULT STDMETHODCALLTYPE Device::EvictManagedResources() {
    FUSE_BRIDGE_LOCK();
    return bridge().post(cmd::IDirect3DDevice9Ex_EvictManagedResources {}, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetDirect3D(IDirect3D9** ppD3D) { return returnObject<IDirect3D9>(parent_, ppD3D); }

HRESULT STDMETHODCALLTYPE Device::GetDeviceCaps(D3DCAPS9* caps) {
    if (caps == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *caps = caps_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetDisplayMode(UINT swapChain, D3DDISPLAYMODE* mode) {
    FUSE_BRIDGE_LOCK();
    if (mode == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_GetDisplayMode c;
    c.swapChain = swapChain;
    cmd::Reply_DisplayMode r;
    if (!bridge().query(c, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    *mode = D3DDISPLAYMODE {r.width, r.height, r.refreshRate, D3DFORMAT(r.format)};
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE Device::GetDisplayModeEx(UINT swapChain, D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_GetDisplayModeEx c;
    c.swapChain = swapChain;
    cmd::Reply_DisplayMode r;
    if (!bridge().query(c, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    if (mode != nullptr) {
        *mode = D3DDISPLAYMODEEX {sizeof(D3DDISPLAYMODEEX), r.width, r.height, r.refreshRate, D3DFORMAT(r.format),
                                  D3DSCANLINEORDERING(r.scanLineOrdering)};
    }
    if (rotation != nullptr) {
        *rotation = D3DDISPLAYROTATION(r.rotation);
    }
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) {
    if (params == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *params = createParams_;
    return D3D_OK;
}

// ---- cursor, swap chains, present ------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* bitmap) {
    FUSE_BRIDGE_LOCK();
    Surface* s = bridgeCast<Surface>(bitmap);
    if (s == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_SetCursorProperties c;
    c.xHotSpot = x;
    c.yHotSpot = y;
    c.cursorBitmap = s->handle();
    return bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

void STDMETHODCALLTYPE Device::SetCursorPosition(int x, int y, DWORD flags) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_SetCursorPosition c;
    c.x = x;
    c.y = y;
    c.flags = flags;
    bridge().post(c, handle());
}

BOOL STDMETHODCALLTYPE Device::ShowCursor(BOOL show) {
    FUSE_BRIDGE_LOCK();
    const BOOL old = cursorVisible_;
    cursorVisible_ = show;
    cmd::IDirect3DDevice9Ex_ShowCursor c;
    c.show = uint32_t(show);
    cmd::Reply_Value r;
    return bridge().query(c, handle(), r) ? BOOL(r.value) : old;
}

HRESULT STDMETHODCALLTYPE Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** ppSwapChain) {
    FUSE_BRIDGE_LOCK();
    if (pp == nullptr || ppSwapChain == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    auto* sc = new SwapChain(newHandle(), this, *pp, ~0u);
    cmd::IDirect3DDevice9Ex_CreateAdditionalSwapChain c;
    c.presentParameters = wire::toWire(*pp);
    c.result = sc->handle();
    const HRESULT hr = bridge().call(c, handle(), D3DERR_INVALIDCALL);
    if (FAILED(hr)) {
        delete sc;
        *ppSwapChain = nullptr;
        return hr;
    }
    sc->addRef();
    *ppSwapChain = sc;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::GetSwapChain(UINT index, IDirect3DSwapChain9** ppSwapChain) {
    if (ppSwapChain == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *ppSwapChain = nullptr;
    if (index != 0 || swapChain_ == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    swapChain_->addRef();
    *ppSwapChain = swapChain_;
    return D3D_OK;
}

UINT STDMETHODCALLTYPE Device::GetNumberOfSwapChains() { return 1; }  // DXVK: one implicit swap chain

HRESULT Device::resetCommon(D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode, bool ex) {
    FUSE_BRIDGE_LOCK();
    if (pp == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (recording_ != nullptr) {
        recording_->releasePrivate();
        recording_ = nullptr;
    }
    resetDeviceState(state_, false);
    releaseImplicit();
    windowDetach();
    HRESULT hr;
    if (ex) {
        cmd::IDirect3DDevice9Ex_ResetEx c;
        c.presentParameters = wire::toWire(*pp);
        c.fullscreenDisplayMode = wire::displayModeExToWire(mode);
        hr = bridge().call(c, handle(), D3DERR_DEVICELOST);
    } else {
        cmd::IDirect3DDevice9Ex_Reset c;
        c.presentParameters = wire::toWire(*pp);
        hr = bridge().call(c, handle(), D3DERR_DEVICELOST);
    }
    // Relink the host's new implicit objects either way (a failed Reset leaves the device lost but
    // the game may still query the back buffer).
    D3DPRESENT_PARAMETERS local = *pp;
    if (local.BackBufferCount == 0) {
        local.BackBufferCount = 1;
    }
    initImplicit(local);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device::Reset(D3DPRESENT_PARAMETERS* pp) { return resetCommon(pp, nullptr, false); }
HRESULT STDMETHODCALLTYPE Device::ResetEx(D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode) {
    return resetCommon(pp, mode, true);
}

HRESULT Device::presentCommon(const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty, DWORD flags, bool ex) {
    FUSE_BRIDGE_LOCK();
    if (!bridge().alive()) {
        return D3DERR_DEVICELOST;
    }
    windowPump();
    // Asynchronous: the link lets the game run ahead of the host by LinkConfig::maxFrameLatency
    // frames (upstream: the Present semaphore) and falls back to in-process DXVK if the host dies.
    if (ex) {
        cmd::IDirect3DDevice9Ex_PresentEx c;
        c.sourceRect = wire::rectToWire(src);
        c.destRect = wire::rectToWire(dst);
        c.destWindowOverride = wire::handleToWire(window);
        c.dirtyRegion = wire::regionToWire(dirty);
        c.flags = flags;
        bridge().send(c, handle(), 0);
    } else {
        cmd::IDirect3DDevice9Ex_Present c;
        c.sourceRect = wire::rectToWire(src);
        c.destRect = wire::rectToWire(dst);
        c.destWindowOverride = wire::handleToWire(window);
        c.dirtyRegion = wire::regionToWire(dirty);
        bridge().send(c, handle(), 0);
    }
    return bridge().alive() ? D3D_OK : D3DERR_DEVICELOST;
}

HRESULT STDMETHODCALLTYPE Device::Present(const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) {
    return presentCommon(src, dst, window, dirty, 0, false);
}

HRESULT STDMETHODCALLTYPE Device::PresentEx(const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty, DWORD flags) {
    return presentCommon(src, dst, window, dirty, flags, true);
}

HRESULT STDMETHODCALLTYPE Device::GetBackBuffer(UINT swapChain, UINT index, D3DBACKBUFFER_TYPE type,
                                                IDirect3DSurface9** ppBackBuffer) {
    FUSE_BRIDGE_LOCK();
    if (swapChain != 0 || swapChain_ == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    return swapChain_->GetBackBuffer(index, type, ppBackBuffer);
}

HRESULT STDMETHODCALLTYPE Device::GetRasterStatus(UINT swapChain, D3DRASTER_STATUS* status) {
    FUSE_BRIDGE_LOCK();
    if (status == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_GetRasterStatus c;
    c.swapChain = swapChain;
    cmd::Reply_RasterStatus r;
    if (!bridge().query(c, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    status->InVBlank = BOOL(r.inVBlank);
    status->ScanLine = r.scanLine;
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE Device::SetDialogBoxMode(BOOL enable) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_SetDialogBoxMode c;
    c.enable = uint32_t(enable);
    return bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

void STDMETHODCALLTYPE Device::SetGammaRamp(UINT swapChain, DWORD flags, const D3DGAMMARAMP* ramp) {
    FUSE_BRIDGE_LOCK();
    if (ramp == nullptr) {
        return;
    }
    gamma_ = *ramp;
    cmd::IDirect3DDevice9Ex_SetGammaRamp c;
    c.swapChain = swapChain;
    c.flags = flags;
    std::memcpy(c.ramp.data(), ramp, sizeof(D3DGAMMARAMP));
    bridge().post(c, handle());
}

void STDMETHODCALLTYPE Device::GetGammaRamp(UINT, D3DGAMMARAMP* ramp) {
    if (ramp != nullptr) {
        *ramp = gamma_;
    }
}

// ---- resource creation -----------------------------------------------------------------------------

namespace {
UINT fullMipCount(UINT w, UINT h, UINT d) {
    UINT m = std::max(w, std::max(h, d));
    UINT n = 1;
    while (m > 1) {
        m >>= 1;
        ++n;
    }
    return n;
}

template <class Obj, class Iface, class Cmd>
HRESULT finishCreate(Obj* o, Cmd& c, uint32_t device, Iface** out) {
    c.result = o->handle();
    const HRESULT hr = bridge().call(c, device, D3DERR_INVALIDCALL);
    if (FAILED(hr)) {
        delete o;
        *out = nullptr;
        return hr;
    }
    o->addRef();
    *out = o;
    return D3D_OK;
}
}  // namespace

HRESULT STDMETHODCALLTYPE Device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                IDirect3DTexture9** ppTexture, HANDLE* shared) {
    FUSE_BRIDGE_LOCK();
    if (ppTexture == nullptr || w == 0 || h == 0 || shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.levels = (usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : (levels == 0 ? fullMipCount(w, h, 1) : levels);
    d.usage = usage;
    d.format = format;
    d.pool = pool;
    cmd::IDirect3DDevice9Ex_CreateTexture c;
    c.width = w;
    c.height = h;
    c.levels = levels;
    c.usage = usage;
    c.format = format;
    c.pool = pool;
    return finishCreate(new Texture(newHandle(), this, d), c, handle(), ppTexture);
}

HRESULT STDMETHODCALLTYPE Device::CreateVolumeTexture(UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
                                                      D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9** ppTexture,
                                                      HANDLE* shared) {
    FUSE_BRIDGE_LOCK();
    if (ppTexture == nullptr || w == 0 || h == 0 || depth == 0 || shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.depth = depth;
    d.levels = levels == 0 ? fullMipCount(w, h, depth) : levels;
    d.usage = usage;
    d.format = format;
    d.pool = pool;
    cmd::IDirect3DDevice9Ex_CreateVolumeTexture c;
    c.width = w;
    c.height = h;
    c.depth = depth;
    c.levels = levels;
    c.usage = usage;
    c.format = format;
    c.pool = pool;
    return finishCreate(new VolumeTexture(newHandle(), this, d), c, handle(), ppTexture);
}

HRESULT STDMETHODCALLTYPE Device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                    IDirect3DCubeTexture9** ppTexture, HANDLE* shared) {
    FUSE_BRIDGE_LOCK();
    if (ppTexture == nullptr || edge == 0 || shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    TextureDesc d;
    d.width = d.height = edge;
    d.levels = (usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : (levels == 0 ? fullMipCount(edge, edge, 1) : levels);
    d.usage = usage;
    d.format = format;
    d.pool = pool;
    cmd::IDirect3DDevice9Ex_CreateCubeTexture c;
    c.edgeLength = edge;
    c.levels = levels;
    c.usage = usage;
    c.format = format;
    c.pool = pool;
    return finishCreate(new CubeTexture(newHandle(), this, d), c, handle(), ppTexture);
}

HRESULT STDMETHODCALLTYPE Device::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                                     IDirect3DVertexBuffer9** ppBuffer, HANDLE* shared) {
    FUSE_BRIDGE_LOCK();
    if (ppBuffer == nullptr || length == 0 || shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    D3DVERTEXBUFFER_DESC d {};
    d.Format = D3DFMT_VERTEXDATA;
    d.Type = D3DRTYPE_VERTEXBUFFER;
    d.Usage = usage;
    d.Pool = pool;
    d.Size = length;
    d.FVF = fvf;
    cmd::IDirect3DDevice9Ex_CreateVertexBuffer c;
    c.length = length;
    c.usage = usage;
    c.fvf = fvf;
    c.pool = pool;
    return finishCreate(new VertexBuffer(newHandle(), this, d), c, handle(), ppBuffer);
}

HRESULT STDMETHODCALLTYPE Device::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                    IDirect3DIndexBuffer9** ppBuffer, HANDLE* shared) {
    FUSE_BRIDGE_LOCK();
    if (ppBuffer == nullptr || length == 0 || shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    D3DINDEXBUFFER_DESC d {};
    d.Format = format;
    d.Type = D3DRTYPE_INDEXBUFFER;
    d.Usage = usage;
    d.Pool = pool;
    d.Size = length;
    cmd::IDirect3DDevice9Ex_CreateIndexBuffer c;
    c.length = length;
    c.usage = usage;
    c.format = format;
    c.pool = pool;
    return finishCreate(new IndexBuffer(newHandle(), this, d), c, handle(), ppBuffer);
}

HRESULT Device::createSurface(uint32_t command, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, DWORD quality,
                              BOOL flag, DWORD usage, D3DPOOL pool, IDirect3DSurface9** ppSurface) {
    FUSE_BRIDGE_LOCK();
    if (ppSurface == nullptr || w == 0 || h == 0) {
        return D3DERR_INVALIDCALL;
    }
    D3DSURFACE_DESC d {};
    d.Format = format;
    d.Type = D3DRTYPE_SURFACE;
    d.Pool = pool;
    d.MultiSampleType = ms;
    d.MultiSampleQuality = quality;
    d.Width = w;
    d.Height = h;
    const auto id = static_cast<schema::CommandId>(command);
    switch (id) {
    case schema::CommandId::IDirect3DDevice9Ex_CreateRenderTarget:
    case schema::CommandId::IDirect3DDevice9Ex_CreateRenderTargetEx: d.Usage = usage | D3DUSAGE_RENDERTARGET; break;
    case schema::CommandId::IDirect3DDevice9Ex_CreateDepthStencilSurface:
    case schema::CommandId::IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx: d.Usage = usage | D3DUSAGE_DEPTHSTENCIL; break;
    default: d.Usage = usage; break;
    }
    auto* s = new Surface(newHandle(), this, d, nullptr, nullptr);
    switch (id) {
    case schema::CommandId::IDirect3DDevice9Ex_CreateRenderTarget: {
        cmd::IDirect3DDevice9Ex_CreateRenderTarget c {};
        c.width = w; c.height = h; c.format = format; c.multiSample = ms; c.multisampleQuality = quality; c.lockable = flag;
        return finishCreate(s, c, handle(), ppSurface);
    }
    case schema::CommandId::IDirect3DDevice9Ex_CreateRenderTargetEx: {
        cmd::IDirect3DDevice9Ex_CreateRenderTargetEx c {};
        c.width = w; c.height = h; c.format = format; c.multiSample = ms; c.multisampleQuality = quality; c.lockable = flag;
        c.usage = usage;
        return finishCreate(s, c, handle(), ppSurface);
    }
    case schema::CommandId::IDirect3DDevice9Ex_CreateDepthStencilSurface: {
        cmd::IDirect3DDevice9Ex_CreateDepthStencilSurface c {};
        c.width = w; c.height = h; c.format = format; c.multiSample = ms; c.multisampleQuality = quality; c.discard = flag;
        return finishCreate(s, c, handle(), ppSurface);
    }
    case schema::CommandId::IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx: {
        cmd::IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx c {};
        c.width = w; c.height = h; c.format = format; c.multiSample = ms; c.multisampleQuality = quality; c.discard = flag;
        c.usage = usage;
        return finishCreate(s, c, handle(), ppSurface);
    }
    case schema::CommandId::IDirect3DDevice9Ex_CreateOffscreenPlainSurface: {
        cmd::IDirect3DDevice9Ex_CreateOffscreenPlainSurface c {};
        c.width = w; c.height = h; c.format = format; c.pool = pool;
        return finishCreate(s, c, handle(), ppSurface);
    }
    default: {
        cmd::IDirect3DDevice9Ex_CreateOffscreenPlainSurfaceEx c {};
        c.width = w; c.height = h; c.format = format; c.pool = pool; c.usage = usage;
        return finishCreate(s, c, handle(), ppSurface);
    }
    }
}

#define FUSE_CMD(x) static_cast<uint32_t>(schema::CommandId::x)

HRESULT STDMETHODCALLTYPE Device::CreateRenderTarget(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, DWORD quality,
                                                     BOOL lockable, IDirect3DSurface9** ppSurface, HANDLE* shared) {
    if (shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    return createSurface(FUSE_CMD(IDirect3DDevice9Ex_CreateRenderTarget), w, h, format, ms, quality, lockable, 0,
                         D3DPOOL_DEFAULT, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                                            DWORD quality, BOOL discard, IDirect3DSurface9** ppSurface,
                                                            HANDLE* shared) {
    if (shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    return createSurface(FUSE_CMD(IDirect3DDevice9Ex_CreateDepthStencilSurface), w, h, format, ms, quality, discard, 0,
                         D3DPOOL_DEFAULT, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT format, D3DPOOL pool,
                                                              IDirect3DSurface9** ppSurface, HANDLE* shared) {
    if (shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    return createSurface(FUSE_CMD(IDirect3DDevice9Ex_CreateOffscreenPlainSurface), w, h, format, D3DMULTISAMPLE_NONE, 0,
                         FALSE, 0, pool, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateRenderTargetEx(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                                       DWORD quality, BOOL lockable, IDirect3DSurface9** ppSurface,
                                                       HANDLE* shared, DWORD usage) {
    if (shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    return createSurface(FUSE_CMD(IDirect3DDevice9Ex_CreateRenderTargetEx), w, h, format, ms, quality, lockable, usage,
                         D3DPOOL_DEFAULT, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateOffscreenPlainSurfaceEx(UINT w, UINT h, D3DFORMAT format, D3DPOOL pool,
                                                                IDirect3DSurface9** ppSurface, HANDLE* shared, DWORD usage) {
    if (shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    return createSurface(FUSE_CMD(IDirect3DDevice9Ex_CreateOffscreenPlainSurfaceEx), w, h, format, D3DMULTISAMPLE_NONE, 0,
                         FALSE, usage, pool, ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::CreateDepthStencilSurfaceEx(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                                              DWORD quality, BOOL discard, IDirect3DSurface9** ppSurface,
                                                              HANDLE* shared, DWORD usage) {
    if (shared != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    return createSurface(FUSE_CMD(IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx), w, h, format, ms, quality, discard,
                         usage, D3DPOOL_DEFAULT, ppSurface);
}

// ---- copies ------------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::UpdateSurface(IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst,
                                                const POINT* dstPoint) {
    FUSE_BRIDGE_LOCK();
    Surface* s = bridgeCast<Surface>(src);
    Surface* d = bridgeCast<Surface>(dst);
    if (s == nullptr || d == nullptr || s == d) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_UpdateSurface c;
    c.source = s->handle();
    c.sourceRect = wire::rectToWire(srcRect);
    c.destination = d->handle();
    c.destPoint = wire::pointToWire(dstPoint);
    d->markHostDirty();
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::UpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) {
    FUSE_BRIDGE_LOCK();
    BridgeObject* s = bridgeObject(src);
    BridgeObject* d = bridgeObject(dst);
    if (s == nullptr || d == nullptr || s->kind() != d->kind()) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_UpdateTexture c;
    c.source = s->handle();
    c.destination = d->handle();
    markTargetDirty(d);
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* dst) {
    FUSE_BRIDGE_LOCK();
    Surface* s = bridgeCast<Surface>(rt);
    Surface* d = bridgeCast<Surface>(dst);
    if (s == nullptr || d == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_GetRenderTargetData c;
    c.renderTarget = s->handle();
    c.destination = d->handle();
    return readWholeSurface(c, handle(), d);
}

HRESULT STDMETHODCALLTYPE Device::GetFrontBufferData(UINT swapChain, IDirect3DSurface9* dst) {
    FUSE_BRIDGE_LOCK();
    Surface* d = bridgeCast<Surface>(dst);
    if (d == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_GetFrontBufferData c;
    c.swapChain = swapChain;
    c.destination = d->handle();
    return readWholeSurface(c, handle(), d);
}

HRESULT STDMETHODCALLTYPE Device::StretchRect(IDirect3DSurface9* src, const RECT* srcRect, IDirect3DSurface9* dst,
                                              const RECT* dstRect, D3DTEXTUREFILTERTYPE filter) {
    FUSE_BRIDGE_LOCK();
    Surface* s = bridgeCast<Surface>(src);
    Surface* d = bridgeCast<Surface>(dst);
    if (s == nullptr || d == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_StretchRect c;
    c.source = s->handle();
    c.sourceRect = wire::rectToWire(srcRect);
    c.destination = d->handle();
    c.destRect = wire::rectToWire(dstRect);
    c.filter = filter;
    d->markHostDirty();
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::ColorFill(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) {
    FUSE_BRIDGE_LOCK();
    Surface* s = bridgeCast<Surface>(surface);
    if (s == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_ColorFill c;
    c.surface = s->handle();
    c.rect = wire::rectToWire(rect);
    c.color = color;
    s->markHostDirty();
    return bridge().post(c, handle());
}

// ---- render targets, scene, clear --------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::SetRenderTarget(DWORD index, IDirect3DSurface9* surface) {
    FUSE_BRIDGE_LOCK();
    Surface* s = bridgeCast<Surface>(surface);
    if (index >= kRenderTargets || (index == 0 && s == nullptr) || (surface != nullptr && s == nullptr)) {
        return D3DERR_INVALIDCALL;
    }
    if (s != nullptr && !(s->desc().Usage & D3DUSAGE_RENDERTARGET)) {
        return D3DERR_INVALIDCALL;
    }
    renderTargets_[index].reset(s);
    if (s != nullptr) {
        s->markGpuWritable();
    }
    if (index == 0) {
        const D3DSURFACE_DESC& d = s->desc();
        state_.viewport = D3DVIEWPORT9 {0, 0, d.Width, d.Height, 0.0f, 1.0f};
        state_.scissorRect = RECT {0, 0, LONG(d.Width), LONG(d.Height)};
    }
    cmd::IDirect3DDevice9Ex_SetRenderTarget c;
    c.index = index;
    c.surface = s ? s->handle() : 0;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetRenderTarget(DWORD index, IDirect3DSurface9** ppSurface) {
    FUSE_BRIDGE_LOCK();
    if (index >= kRenderTargets || ppSurface == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    return returnObject<IDirect3DSurface9>(renderTargets_[index].get(), ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::SetDepthStencilSurface(IDirect3DSurface9* surface) {
    FUSE_BRIDGE_LOCK();
    Surface* s = bridgeCast<Surface>(surface);
    if (surface != nullptr && s == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    depthStencil_.reset(s);
    cmd::IDirect3DDevice9Ex_SetDepthStencilSurface c;
    c.surface = s ? s->handle() : 0;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetDepthStencilSurface(IDirect3DSurface9** ppSurface) {
    FUSE_BRIDGE_LOCK();
    return returnObject<IDirect3DSurface9>(depthStencil_.get(), ppSurface);
}

HRESULT STDMETHODCALLTYPE Device::BeginScene() {
    FUSE_BRIDGE_LOCK();
    if (inScene_) {
        return D3DERR_INVALIDCALL;
    }
    inScene_ = true;
    return bridge().post(cmd::IDirect3DDevice9Ex_BeginScene {}, handle());
}

HRESULT STDMETHODCALLTYPE Device::EndScene() {
    FUSE_BRIDGE_LOCK();
    if (!inScene_) {
        return D3DERR_INVALIDCALL;
    }
    inScene_ = false;
    return bridge().post(cmd::IDirect3DDevice9Ex_EndScene {}, handle());
}

HRESULT STDMETHODCALLTYPE Device::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                                        DWORD stencil) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_Clear c;
    if (rects != nullptr && count != 0) {
        c.rects.resize(size_t(count) * 4);
        std::memcpy(c.rects.data(), rects, size_t(count) * sizeof(D3DRECT));
    }
    c.flags = flags;
    c.color = color;
    c.z = z;
    c.stencil = stencil;
    return bridge().post(c, handle());
}

// ---- fixed-function state ------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    FUSE_BRIDGE_LOCK();
    uint32_t slot;
    if (matrix == nullptr || !transformSlot(state, slot)) {
        return D3DERR_INVALIDCALL;
    }
    target().transforms[slot] = *matrix;
    recordMask([&](StateMask& m) { m.transforms.set(slot); });
    cmd::IDirect3DDevice9Ex_SetTransform c;
    c.state = state;
    std::memcpy(c.matrix.data(), matrix, sizeof(D3DMATRIX));
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix) {
    FUSE_BRIDGE_LOCK();
    uint32_t slot;
    if (matrix == nullptr || !transformSlot(state, slot)) {
        return D3DERR_INVALIDCALL;
    }
    *matrix = state_.transforms[slot];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    FUSE_BRIDGE_LOCK();
    uint32_t slot;
    if (matrix == nullptr || !transformSlot(state, slot)) {
        return D3DERR_INVALIDCALL;
    }
    // D3D9: M' = M * current (DXVK: ConvertMatrix(pMatrix) * current), in float as DXVK does.
    const D3DMATRIX& a = *matrix;
    const D3DMATRIX b = target().transforms[slot];
    D3DMATRIX r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        }
    }
    target().transforms[slot] = r;
    recordMask([&](StateMask& m) { m.transforms.set(slot); });
    cmd::IDirect3DDevice9Ex_MultiplyTransform c;
    c.state = state;
    std::memcpy(c.matrix.data(), matrix, sizeof(D3DMATRIX));
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::SetViewport(const D3DVIEWPORT9* viewport) {
    FUSE_BRIDGE_LOCK();
    if (viewport == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    target().viewport = *viewport;
    recordMask([&](StateMask& m) { m.viewport = true; });
    cmd::IDirect3DDevice9Ex_SetViewport c;
    c.x = viewport->X;
    c.y = viewport->Y;
    c.width = viewport->Width;
    c.height = viewport->Height;
    c.minZ = viewport->MinZ;
    c.maxZ = viewport->MaxZ;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetViewport(D3DVIEWPORT9* viewport) {
    if (viewport == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *viewport = state_.viewport;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetMaterial(const D3DMATERIAL9* material) {
    FUSE_BRIDGE_LOCK();
    if (material == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    target().material = *material;
    recordMask([&](StateMask& m) { m.material = true; });
    cmd::IDirect3DDevice9Ex_SetMaterial c;
    std::memcpy(c.material.data(), material, sizeof(D3DMATERIAL9));
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetMaterial(D3DMATERIAL9* material) {
    if (material == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *material = state_.material;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetLight(DWORD index, const D3DLIGHT9* light) {
    FUSE_BRIDGE_LOCK();
    if (light == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    target().lights[index] = *light;
    recordMask([&](StateMask& m) { m.lights[index] = true; });
    cmd::IDirect3DDevice9Ex_SetLight c;
    c.index = index;
    c.type = light->Type;
    c.parameters = wire::lightToWire(*light);
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetLight(DWORD index, D3DLIGHT9* light) {
    FUSE_BRIDGE_LOCK();
    auto it = state_.lights.find(index);
    if (light == nullptr || it == state_.lights.end()) {
        return D3DERR_INVALIDCALL;
    }
    *light = it->second;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::LightEnable(DWORD index, BOOL enable) {
    FUSE_BRIDGE_LOCK();
    DeviceState& t = target();
    if (t.lights.find(index) == t.lights.end()) {
        // D3D9: enabling an unset light creates the default directional light.
        D3DLIGHT9 def {};
        def.Type = D3DLIGHT_DIRECTIONAL;
        def.Diffuse = D3DCOLORVALUE {1.0f, 1.0f, 1.0f, 0.0f};
        def.Direction = D3DVECTOR {0.0f, 0.0f, 1.0f};
        t.lights[index] = def;
    }
    t.lightEnables[index] = enable ? TRUE : FALSE;
    recordMask([&](StateMask& m) {
        m.lights[index] = true;
        m.lightEnables[index] = true;
    });
    cmd::IDirect3DDevice9Ex_LightEnable c;
    c.index = index;
    c.enable = uint32_t(enable);
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetLightEnable(DWORD index, BOOL* enable) {
    FUSE_BRIDGE_LOCK();
    auto it = state_.lightEnables.find(index);
    if (enable == nullptr || state_.lights.find(index) == state_.lights.end()) {
        return D3DERR_INVALIDCALL;
    }
    *enable = (it != state_.lightEnables.end() && it->second) ? 128 : 0;  // DXVK/D3D9 report 128
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetClipPlane(DWORD index, const float* plane) {
    FUSE_BRIDGE_LOCK();
    if (plane == nullptr || index >= kClipPlanes) {
        return D3DERR_INVALIDCALL;
    }
    std::memcpy(target().clipPlanes[index].data(), plane, 4 * sizeof(float));
    recordMask([&](StateMask& m) { m.clipPlanes.set(index); });
    cmd::IDirect3DDevice9Ex_SetClipPlane c;
    c.index = index;
    std::memcpy(c.plane.data(), plane, 4 * sizeof(float));
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetClipPlane(DWORD index, float* plane) {
    if (plane == nullptr || index >= kClipPlanes) {
        return D3DERR_INVALIDCALL;
    }
    std::memcpy(plane, state_.clipPlanes[index].data(), 4 * sizeof(float));
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    FUSE_BRIDGE_LOCK();
    if (uint32_t(state) >= kRenderStates) {
        return D3D_OK;  // D3D9 ignores unknown states
    }
    target().renderStates[state] = value;
    recordMask([&](StateMask& m) { m.renderStates.set(state); });
    cmd::IDirect3DDevice9Ex_SetRenderState c;
    c.state = state;
    c.value = value;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) {
    if (value == nullptr || uint32_t(state) >= kRenderStates) {
        return D3DERR_INVALIDCALL;
    }
    *value = state_.renderStates[state];
    return D3D_OK;
}

// ---- state blocks ----------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::CreateStateBlock(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** ppBlock) {
    FUSE_BRIDGE_LOCK();
    if (ppBlock == nullptr || (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE && type != D3DSBT_VERTEXSTATE)) {
        return D3DERR_INVALIDCALL;
    }
    auto* sb = new StateBlock(newHandle(), this, softwareVpConsts_);
    stateBlockMask(type, sb->mask);
    transferState(sb->mask, state_, sb->state);
    cmd::IDirect3DDevice9Ex_CreateStateBlock c;
    c.type = type;
    return finishCreate(sb, c, handle(), ppBlock);
}

HRESULT STDMETHODCALLTYPE Device::BeginStateBlock() {
    FUSE_BRIDGE_LOCK();
    if (recording_ != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    recording_ = new StateBlock(newHandle(), this, softwareVpConsts_);
    recording_->addRefPrivate();
    // Unrecorded values of the block are the device's (so Apply of a partially recorded block only
    // changes what was recorded; the mask decides).
    return bridge().post(cmd::IDirect3DDevice9Ex_BeginStateBlock {}, handle());
}

HRESULT STDMETHODCALLTYPE Device::EndStateBlock(IDirect3DStateBlock9** ppBlock) {
    FUSE_BRIDGE_LOCK();
    if (ppBlock == nullptr || recording_ == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    StateBlock* sb = recording_;
    recording_ = nullptr;
    cmd::IDirect3DDevice9Ex_EndStateBlock c;
    c.result = sb->handle();
    const HRESULT hr = bridge().post(c, handle());
    sb->addRef();
    sb->releasePrivate();
    *ppBlock = sb;
    return hr;
}

HRESULT STDMETHODCALLTYPE Device::SetClipStatus(const D3DCLIPSTATUS9* status) {
    FUSE_BRIDGE_LOCK();
    if (status == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    clipStatus_ = *status;
    cmd::IDirect3DDevice9Ex_SetClipStatus c;
    c.clipUnion = status->ClipUnion;
    c.clipIntersection = status->ClipIntersection;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetClipStatus(D3DCLIPSTATUS9* status) {
    if (status == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *status = clipStatus_;
    return D3D_OK;
}

// ---- textures, stage and sampler state ----------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::GetTexture(DWORD stage, IDirect3DBaseTexture9** ppTexture) {
    FUSE_BRIDGE_LOCK();
    uint32_t slot;
    if (ppTexture == nullptr || !samplerSlot(stage, slot)) {
        return D3DERR_INVALIDCALL;
    }
    BridgeObject* t = state_.textures[slot].get();
    *ppTexture = asIface<IDirect3DBaseTexture9>(t);
    if (t != nullptr) {
        t->addRef();
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) {
    FUSE_BRIDGE_LOCK();
    uint32_t slot;
    BridgeObject* t = bridgeObject(texture);
    if (!samplerSlot(stage, slot) || (texture != nullptr && t == nullptr)) {
        return D3DERR_INVALIDCALL;
    }
    target().textures[slot].reset(t);
    recordMask([&](StateMask& m) { m.textures.set(slot); });
    cmd::IDirect3DDevice9Ex_SetTexture c;
    c.stage = stage;
    c.texture = t ? t->handle() : 0;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) {
    if (value == nullptr || stage >= kStages || uint32_t(type) >= kStageStates) {
        return D3DERR_INVALIDCALL;
    }
    *value = state_.stageStates[stage][type];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    FUSE_BRIDGE_LOCK();
    if (stage >= kStages || uint32_t(type) >= kStageStates) {
        return D3DERR_INVALIDCALL;
    }
    target().stageStates[stage][type] = value;
    recordMask([&](StateMask& m) { m.stageStates[stage].set(type); });
    cmd::IDirect3DDevice9Ex_SetTextureStageState c;
    c.stage = stage;
    c.type = type;
    c.value = value;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) {
    uint32_t slot;
    if (value == nullptr || !samplerSlot(sampler, slot) || uint32_t(type) >= kSamplerStates) {
        return D3DERR_INVALIDCALL;
    }
    *value = state_.samplerStates[slot][type];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    FUSE_BRIDGE_LOCK();
    uint32_t slot;
    if (!samplerSlot(sampler, slot) || uint32_t(type) >= kSamplerStates) {
        return D3DERR_INVALIDCALL;
    }
    target().samplerStates[slot][type] = value;
    recordMask([&](StateMask& m) { m.samplerStates[slot].set(type); });
    cmd::IDirect3DDevice9Ex_SetSamplerState c;
    c.sampler = sampler;
    c.type = type;
    c.value = value;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::ValidateDevice(DWORD* passes) {
    FUSE_BRIDGE_LOCK();
    cmd::Reply_Value r;
    if (!bridge().query(cmd::IDirect3DDevice9Ex_ValidateDevice {}, handle(), r)) {
        return D3DERR_DEVICELOST;
    }
    if (passes != nullptr) {
        *passes = r.value;
    }
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE Device::SetPaletteEntries(UINT palette, const PALETTEENTRY* entries) {
    FUSE_BRIDGE_LOCK();
    if (entries == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    std::memcpy(palettes_[palette].data(), entries, 256 * sizeof(PALETTEENTRY));
    cmd::IDirect3DDevice9Ex_SetPaletteEntries c;
    c.paletteNumber = palette;
    std::memcpy(c.entries.data(), entries, 256 * sizeof(PALETTEENTRY));
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetPaletteEntries(UINT palette, PALETTEENTRY* entries) {
    FUSE_BRIDGE_LOCK();
    auto it = palettes_.find(palette);
    if (entries == nullptr || it == palettes_.end()) {
        return D3DERR_INVALIDCALL;
    }
    std::memcpy(entries, it->second.data(), 256 * sizeof(PALETTEENTRY));
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetCurrentTexturePalette(UINT palette) {
    FUSE_BRIDGE_LOCK();
    currentPalette_ = palette;
    cmd::IDirect3DDevice9Ex_SetCurrentTexturePalette c;
    c.paletteNumber = palette;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetCurrentTexturePalette(UINT* palette) {
    if (palette == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *palette = currentPalette_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetScissorRect(const RECT* rect) {
    FUSE_BRIDGE_LOCK();
    if (rect == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    target().scissorRect = *rect;
    recordMask([&](StateMask& m) { m.scissorRect = true; });
    cmd::IDirect3DDevice9Ex_SetScissorRect c;
    c.rect = {rect->left, rect->top, rect->right, rect->bottom};
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetScissorRect(RECT* rect) {
    if (rect == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *rect = state_.scissorRect;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetSoftwareVertexProcessing(BOOL software) {
    FUSE_BRIDGE_LOCK();
    if (software && !(createParams_.BehaviorFlags & (D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_SOFTWARE_VERTEXPROCESSING))) {
        return D3DERR_INVALIDCALL;
    }
    softwareVp_ = software;
    cmd::IDirect3DDevice9Ex_SetSoftwareVertexProcessing c;
    c.software = uint32_t(software);
    return bridge().post(c, handle());
}

BOOL STDMETHODCALLTYPE Device::GetSoftwareVertexProcessing() { return softwareVp_; }

HRESULT STDMETHODCALLTYPE Device::SetNPatchMode(float segments) {
    FUSE_BRIDGE_LOCK();
    nPatch_ = segments;
    cmd::IDirect3DDevice9Ex_SetNPatchMode c;
    c.segments = segments;
    return bridge().post(c, handle());
}

float STDMETHODCALLTYPE Device::GetNPatchMode() { return nPatch_; }

// ---- draws ---------------------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_DrawPrimitive c;
    c.primitiveType = type;
    c.startVertex = start;
    c.primitiveCount = count;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex,
                                                       UINT numVertices, UINT startIndex, UINT count) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_DrawIndexedPrimitive c;
    c.primitiveType = type;
    c.baseVertexIndex = baseVertex;
    c.minVertexIndex = minIndex;
    c.numVertices = numVertices;
    c.startIndex = startIndex;
    c.primitiveCount = count;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
    FUSE_BRIDGE_LOCK();
    if (data == nullptr || stride == 0) {
        return D3DERR_INVALIDCALL;
    }
    const uint32_t vertices = vertexCount(type, count);
    const size_t bytes = size_t(vertices) * stride;
    HRESULT hr;
    if (bytes <= kUpInlineMax) {
        cmd::IDirect3DDevice9Ex_DrawPrimitiveUP c;
        c.primitiveType = type;
        c.primitiveCount = count;
        c.vertexStreamZeroStride = stride;
        c.vertexData.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
        hr = bridge().post(c, handle());
    } else {
        // Too big for the command ring: an indexed UP draw with the identity index list, whose
        // payload the link moves to the shared heap (and journals inline).
        std::vector<uint8_t> payload(size_t(vertices) * 4 + bytes);
        for (uint32_t i = 0; i < vertices; ++i) {
            std::memcpy(payload.data() + size_t(i) * 4, &i, 4);
        }
        std::memcpy(payload.data() + size_t(vertices) * 4, data, bytes);
        cmd::IDirect3DDevice9Ex_DrawIndexedPrimitiveUP c;
        c.primitiveType = type;
        c.minVertexIndex = 0;
        c.numVertices = vertices;
        c.primitiveCount = count;
        c.indexDataFormat = D3DFMT_INDEX32;
        c.vertexStreamZeroStride = stride;
        c.indexBytes = vertices * 4;
        bridge().attachData(c, payload.data(), payload.size());
        hr = bridge().post(c, handle());
    }
    // D3D9: UP draws leave stream 0 unbound.
    state_.streams[0].reset(nullptr);
    state_.streamOffsets[0] = 0;
    state_.streamStrides[0] = 0;
    return hr;
}

HRESULT STDMETHODCALLTYPE Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT count,
                                                         const void* indices, D3DFORMAT indexFormat, const void* data,
                                                         UINT stride) {
    FUSE_BRIDGE_LOCK();
    if (data == nullptr || indices == nullptr || stride == 0 ||
        (indexFormat != D3DFMT_INDEX16 && indexFormat != D3DFMT_INDEX32)) {
        return D3DERR_INVALIDCALL;
    }
    const size_t indexBytes = size_t(vertexCount(type, count)) * (indexFormat == D3DFMT_INDEX16 ? 2 : 4);
    const size_t vertexBytes = size_t(minIndex + numVertices) * stride;
    std::vector<uint8_t> payload(indexBytes + vertexBytes);
    std::memcpy(payload.data(), indices, indexBytes);
    std::memcpy(payload.data() + indexBytes, data, vertexBytes);
    cmd::IDirect3DDevice9Ex_DrawIndexedPrimitiveUP c;
    c.primitiveType = type;
    c.minVertexIndex = minIndex;
    c.numVertices = numVertices;
    c.primitiveCount = count;
    c.indexDataFormat = indexFormat;
    c.vertexStreamZeroStride = stride;
    c.indexBytes = uint32_t(indexBytes);
    if (!bridge().attachData(c, payload.data(), payload.size())) {
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    const HRESULT hr = bridge().post(c, handle());
    state_.streams[0].reset(nullptr);
    state_.streamOffsets[0] = 0;
    state_.streamStrides[0] = 0;
    state_.indices.reset(nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Device::ProcessVertices(UINT srcStart, UINT dstIndex, UINT count, IDirect3DVertexBuffer9* dst,
                                                  IDirect3DVertexDeclaration9* decl, DWORD flags) {
    FUSE_BRIDGE_LOCK();
    auto* d = bridgeCast<VertexBuffer>(dst);
    if (d == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_ProcessVertices c;
    c.srcStartIndex = srcStart;
    c.destIndex = dstIndex;
    c.vertexCount = count;
    c.destBuffer = d->handle();
    c.vertexDecl = handleOf(decl);
    c.flags = flags;
    d->markHostDirty();
    return bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

// ---- vertex declarations, FVF, shaders ------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements,
                                                          IDirect3DVertexDeclaration9** ppDecl) {
    FUSE_BRIDGE_LOCK();
    if (elements == nullptr || ppDecl == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    size_t n = 0;
    while (n < MAXD3DDECLLENGTH && elements[n].Stream != 0xFF) {
        ++n;
    }
    if (n >= MAXD3DDECLLENGTH) {
        return D3DERR_INVALIDCALL;
    }
    std::vector<D3DVERTEXELEMENT9> v(elements, elements + n + 1);
    cmd::IDirect3DDevice9Ex_CreateVertexDeclaration c;
    c.elements = toWords(v.data(), v.size() * wire::kVertexElementWords);
    return finishCreate(new VertexDeclaration(newHandle(), this, std::move(v)), c, handle(), ppDecl);
}

HRESULT STDMETHODCALLTYPE Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* decl) {
    FUSE_BRIDGE_LOCK();
    BridgeObject* d = bridgeObject(decl);
    if (decl != nullptr && d == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    target().vertexDecl.reset(d);
    recordMask([&](StateMask& m) { m.vertexDecl = true; });
    cmd::IDirect3DDevice9Ex_SetVertexDeclaration c;
    c.declaration = d ? d->handle() : 0;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) {
    FUSE_BRIDGE_LOCK();
    if (ppDecl == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    BridgeObject* d = state_.vertexDecl.get();
    *ppDecl = asIface<IDirect3DVertexDeclaration9>(d);
    if (d != nullptr) {
        d->addRef();
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetFVF(DWORD fvf) {
    FUSE_BRIDGE_LOCK();
    // DXVK/D3D9 implement SetFVF as an implicit vertex declaration; GetVertexDeclaration then
    // returns an internal object the client does not model, so it reports none.
    target().fvf = fvf;
    target().vertexDecl.reset(nullptr);
    recordMask([&](StateMask& m) { m.vertexDecl = true; });
    cmd::IDirect3DDevice9Ex_SetFVF c;
    c.fvf = fvf;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetFVF(DWORD* fvf) {
    if (fvf == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *fvf = state_.vertexDecl ? 0 : state_.fvf;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreateVertexShader(const DWORD* code, IDirect3DVertexShader9** ppShader) {
    FUSE_BRIDGE_LOCK();
    if (code == nullptr || ppShader == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const size_t n = shaderLength(code);
    if (n == 0) {
        return D3DERR_INVALIDCALL;
    }
    std::vector<DWORD> v(code, code + n);
    cmd::IDirect3DDevice9Ex_CreateVertexShader c;
    c.function = toWords(v.data(), n);
    return finishCreate(new VertexShader(newHandle(), this, std::move(v)), c, handle(), ppShader);
}

HRESULT STDMETHODCALLTYPE Device::SetVertexShader(IDirect3DVertexShader9* shader) {
    FUSE_BRIDGE_LOCK();
    BridgeObject* s = bridgeObject(shader);
    if (shader != nullptr && s == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    target().vertexShader.reset(s);
    recordMask([&](StateMask& m) { m.vertexShader = true; });
    cmd::IDirect3DDevice9Ex_SetVertexShader c;
    c.shader = s ? s->handle() : 0;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetVertexShader(IDirect3DVertexShader9** ppShader) {
    FUSE_BRIDGE_LOCK();
    if (ppShader == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    BridgeObject* s = state_.vertexShader.get();
    *ppShader = asIface<IDirect3DVertexShader9>(s);
    if (s != nullptr) {
        s->addRef();
    }
    return D3D_OK;
}

namespace {
template <class T, class Cmd>
HRESULT setConstants(std::vector<T>& dst, std::vector<bool>* mask, UINT start, const T* data, UINT count, UINT per,
                     Cmd& c, uint32_t device) {
    if (data == nullptr || size_t(start + count) * per > dst.size()) {
        return D3DERR_INVALIDCALL;
    }
    std::memcpy(dst.data() + size_t(start) * per, data, size_t(count) * per * sizeof(T));
    if (mask != nullptr) {
        for (UINT i = 0; i < count && start + i < mask->size(); ++i) {
            (*mask)[start + i] = true;
        }
    }
    static_assert(sizeof(typename decltype(c.data)::value_type) == sizeof(T), "wire element size");
    c.startRegister = start;
    c.data.resize(size_t(count) * per);
    std::memcpy(c.data.data(), data, size_t(count) * per * sizeof(T));
    return bridge().post(c, device);
}
template <class T>
HRESULT getConstants(const std::vector<T>& src, UINT start, T* data, UINT count, UINT per) {
    if (data == nullptr || size_t(start + count) * per > src.size()) {
        return D3DERR_INVALIDCALL;
    }
    std::memcpy(data, src.data() + size_t(start) * per, size_t(count) * per * sizeof(T));
    return D3D_OK;
}
}  // namespace

HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstantF(UINT start, const float* data, UINT count) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_SetVertexShaderConstantF c;
    return setConstants(target().vsFloat, recording_ ? &recording_->mask.vsFloat : nullptr, start, data, count, 4, c, handle());
}
HRESULT STDMETHODCALLTYPE Device::GetVertexShaderConstantF(UINT start, float* data, UINT count) {
    return getConstants(state_.vsFloat, start, data, count, 4);
}
HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstantI(UINT start, const int* data, UINT count) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_SetVertexShaderConstantI c;
    static_assert(sizeof(int) == sizeof(int32_t));
    return setConstants(target().vsInt, recording_ ? &recording_->mask.vsInt : nullptr, start, data, count, 4, c, handle());
}
HRESULT STDMETHODCALLTYPE Device::GetVertexShaderConstantI(UINT start, int* data, UINT count) {
    return getConstants(state_.vsInt, start, data, count, 4);
}
HRESULT STDMETHODCALLTYPE Device::SetVertexShaderConstantB(UINT start, const BOOL* data, UINT count) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_SetVertexShaderConstantB c;
    return setConstants(target().vsBool, recording_ ? &recording_->mask.vsBool : nullptr, start, data, count, 1, c, handle());
}
HRESULT STDMETHODCALLTYPE Device::GetVertexShaderConstantB(UINT start, BOOL* data, UINT count) {
    return getConstants(state_.vsBool, start, data, count, 1);
}
HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstantF(UINT start, const float* data, UINT count) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_SetPixelShaderConstantF c;
    return setConstants(target().psFloat, recording_ ? &recording_->mask.psFloat : nullptr, start, data, count, 4, c, handle());
}
HRESULT STDMETHODCALLTYPE Device::GetPixelShaderConstantF(UINT start, float* data, UINT count) {
    return getConstants(state_.psFloat, start, data, count, 4);
}
HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstantI(UINT start, const int* data, UINT count) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_SetPixelShaderConstantI c;
    return setConstants(target().psInt, recording_ ? &recording_->mask.psInt : nullptr, start, data, count, 4, c, handle());
}
HRESULT STDMETHODCALLTYPE Device::GetPixelShaderConstantI(UINT start, int* data, UINT count) {
    return getConstants(state_.psInt, start, data, count, 4);
}
HRESULT STDMETHODCALLTYPE Device::SetPixelShaderConstantB(UINT start, const BOOL* data, UINT count) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_SetPixelShaderConstantB c;
    return setConstants(target().psBool, recording_ ? &recording_->mask.psBool : nullptr, start, data, count, 1, c, handle());
}
HRESULT STDMETHODCALLTYPE Device::GetPixelShaderConstantB(UINT start, BOOL* data, UINT count) {
    return getConstants(state_.psBool, start, data, count, 1);
}

// ---- streams, indices, pixel shaders --------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::SetStreamSource(UINT stream, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) {
    FUSE_BRIDGE_LOCK();
    BridgeObject* b = bridgeObject(buffer);
    if (stream >= kStreams || (buffer != nullptr && b == nullptr)) {
        return D3DERR_INVALIDCALL;
    }
    DeviceState& t = target();
    t.streams[stream].reset(b);
    t.streamOffsets[stream] = offset;
    t.streamStrides[stream] = stride;
    recordMask([&](StateMask& m) { m.streams.set(stream); });
    cmd::IDirect3DDevice9Ex_SetStreamSource c;
    c.streamNumber = stream;
    c.vertexBuffer = b ? b->handle() : 0;
    c.offsetInBytes = offset;
    c.stride = stride;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetStreamSource(UINT stream, IDirect3DVertexBuffer9** ppBuffer, UINT* offset, UINT* stride) {
    FUSE_BRIDGE_LOCK();
    if (stream >= kStreams || ppBuffer == nullptr || offset == nullptr || stride == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    BridgeObject* b = state_.streams[stream].get();
    *ppBuffer = asIface<IDirect3DVertexBuffer9>(b);
    if (b != nullptr) {
        b->addRef();
    }
    *offset = state_.streamOffsets[stream];
    *stride = state_.streamStrides[stream];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetStreamSourceFreq(UINT stream, UINT setting) {
    FUSE_BRIDGE_LOCK();
    if (stream >= kStreams) {
        return D3DERR_INVALIDCALL;
    }
    target().streamFreqs[stream] = setting;
    recordMask([&](StateMask& m) { m.streamFreqs.set(stream); });
    cmd::IDirect3DDevice9Ex_SetStreamSourceFreq c;
    c.streamNumber = stream;
    c.setting = setting;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetStreamSourceFreq(UINT stream, UINT* setting) {
    if (stream >= kStreams || setting == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *setting = state_.streamFreqs[stream];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetIndices(IDirect3DIndexBuffer9* buffer) {
    FUSE_BRIDGE_LOCK();
    BridgeObject* b = bridgeObject(buffer);
    if (buffer != nullptr && b == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    target().indices.reset(b);
    recordMask([&](StateMask& m) { m.indices = true; });
    cmd::IDirect3DDevice9Ex_SetIndices c;
    c.indexBuffer = b ? b->handle() : 0;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetIndices(IDirect3DIndexBuffer9** ppBuffer) {
    FUSE_BRIDGE_LOCK();
    if (ppBuffer == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    BridgeObject* b = state_.indices.get();
    *ppBuffer = asIface<IDirect3DIndexBuffer9>(b);
    if (b != nullptr) {
        b->addRef();
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CreatePixelShader(const DWORD* code, IDirect3DPixelShader9** ppShader) {
    FUSE_BRIDGE_LOCK();
    if (code == nullptr || ppShader == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const size_t n = shaderLength(code);
    if (n == 0) {
        return D3DERR_INVALIDCALL;
    }
    std::vector<DWORD> v(code, code + n);
    cmd::IDirect3DDevice9Ex_CreatePixelShader c;
    c.function = toWords(v.data(), n);
    return finishCreate(new PixelShader(newHandle(), this, std::move(v)), c, handle(), ppShader);
}

HRESULT STDMETHODCALLTYPE Device::SetPixelShader(IDirect3DPixelShader9* shader) {
    FUSE_BRIDGE_LOCK();
    BridgeObject* s = bridgeObject(shader);
    if (shader != nullptr && s == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    target().pixelShader.reset(s);
    recordMask([&](StateMask& m) { m.pixelShader = true; });
    cmd::IDirect3DDevice9Ex_SetPixelShader c;
    c.shader = s ? s->handle() : 0;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetPixelShader(IDirect3DPixelShader9** ppShader) {
    FUSE_BRIDGE_LOCK();
    if (ppShader == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    BridgeObject* s = state_.pixelShader.get();
    *ppShader = asIface<IDirect3DPixelShader9>(s);
    if (s != nullptr) {
        s->addRef();
    }
    return D3D_OK;
}

// ---- patches, queries -------------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::DrawRectPatch(UINT handleId, const float* segs, const D3DRECTPATCH_INFO* info) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_DrawRectPatch c;
    c.patchHandle = handleId;
    if (segs != nullptr) {
        c.numSegs.assign(segs, segs + 4);
    }
    c.rectPatchInfo = wire::wordsToWire<D3DRECTPATCH_INFO, wire::kRectPatchWords>(info);
    return bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

HRESULT STDMETHODCALLTYPE Device::DrawTriPatch(UINT handleId, const float* segs, const D3DTRIPATCH_INFO* info) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_DrawTriPatch c;
    c.patchHandle = handleId;
    if (segs != nullptr) {
        c.numSegs.assign(segs, segs + 3);
    }
    c.triPatchInfo = wire::wordsToWire<D3DTRIPATCH_INFO, wire::kTriPatchWords>(info);
    return bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

HRESULT STDMETHODCALLTYPE Device::DeletePatch(UINT handleId) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_DeletePatch c;
    c.patchHandle = handleId;
    return bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

HRESULT STDMETHODCALLTYPE Device::CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9** ppQuery) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_CreateQuery c;
    c.type = type;
    if (ppQuery == nullptr) {
        c.result = 0;  // support check
        return bridge().call(c, handle(), D3DERR_NOTAVAILABLE);
    }
    return finishCreate(new Query(newHandle(), this, type), c, handle(), ppQuery);
}

// ---- IDirect3DDevice9Ex ------------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Device::SetConvolutionMonoKernel(UINT w, UINT h, float* rows, float* columns) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_SetConvolutionMonoKernel c;
    c.width = w;
    c.height = h;
    if (rows != nullptr) {
        c.rows.assign(rows, rows + w);
    }
    if (columns != nullptr) {
        c.columns.assign(columns, columns + h);
    }
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::ComposeRects(IDirect3DSurface9* src, IDirect3DSurface9* dst, IDirect3DVertexBuffer9* srcDescs,
                                               UINT count, IDirect3DVertexBuffer9* dstDescs, D3DCOMPOSERECTSOP op, INT x,
                                               INT y) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_ComposeRects c;
    c.source = handleOf(src);
    c.destination = handleOf(dst);
    c.srcRectDescs = handleOf(srcDescs);
    c.numRects = count;
    c.dstRectDescs = handleOf(dstDescs);
    c.operation = op;
    c.xOffset = x;
    c.yOffset = y;
    markTargetDirty(bridgeObject(dst));
    return bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

HRESULT STDMETHODCALLTYPE Device::GetGPUThreadPriority(INT* priority) {
    if (priority == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *priority = gpuPriority_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::SetGPUThreadPriority(INT priority) {
    FUSE_BRIDGE_LOCK();
    if (priority < -7 || priority > 7) {
        return D3DERR_INVALIDCALL;
    }
    gpuPriority_ = priority;
    cmd::IDirect3DDevice9Ex_SetGPUThreadPriority c;
    c.priority = priority;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::WaitForVBlank(UINT swapChain) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_WaitForVBlank c;
    c.swapChain = swapChain;
    return bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

HRESULT STDMETHODCALLTYPE Device::CheckResourceResidency(IDirect3DResource9** resources, UINT32 count) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_CheckResourceResidency c;
    for (UINT32 i = 0; resources != nullptr && i < count; ++i) {
        c.resources.push_back(handleOf(resources[i]));
    }
    return bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

HRESULT STDMETHODCALLTYPE Device::SetMaximumFrameLatency(UINT latency) {
    FUSE_BRIDGE_LOCK();
    maxLatency_ = latency == 0 ? 3 : std::min<UINT>(latency, 20);
    cmd::IDirect3DDevice9Ex_SetMaximumFrameLatency c;
    c.maxLatency = latency;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Device::GetMaximumFrameLatency(UINT* latency) {
    if (latency == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *latency = maxLatency_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Device::CheckDeviceState(HWND window) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DDevice9Ex_CheckDeviceState c;
    c.destinationWindow = wire::handleToWire(window);
    return bridge().call(c, handle(), D3DERR_DEVICELOST);
}

// ---- DXVK d3d8 interop (IDxvkLegacyD3DDeviceBridge) ------------------------------------------------------------

HRESULT Device::updateTextureFromBuffer(IDirect3DSurface9* dst, IDirect3DSurface9* src, const RECT* srcRect,
                                        const POINT* dstPoint) {
    FUSE_BRIDGE_LOCK();
    Surface* d = bridgeCast<Surface>(dst);
    Surface* s = bridgeCast<Surface>(src);
    if (d == nullptr || s == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DDevice9Ex_UpdateTextureFromBuffer c;
    c.destination = d->handle();
    c.source = s->handle();
    c.sourceRect = wire::rectToWire(srcRect);
    c.destPoint = wire::pointToWire(dstPoint);
    d->markHostDirty();
    return bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

bool Device::isSupportedSurfaceFormat(D3DFORMAT format) {
    FUSE_BRIDGE_LOCK();
    auto it = surfaceFormats_.find(format);
    if (it != surfaceFormats_.end()) {
        return it->second;
    }
    cmd::IDirect3DDevice9Ex_IsSupportedSurfaceFormat c;
    c.format = format;
    cmd::Reply_Value r;
    const bool ok = bridge().query(c, handle(), r) && r.value != 0;
    surfaceFormats_[format] = ok;
    return ok;
}

}  // namespace fuse::relight::bridge::client
