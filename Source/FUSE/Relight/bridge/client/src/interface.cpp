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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix bridge/src/client/{d3d9_module.cpp,d3d9_bootstrap.cpp,d3d9_lss.cpp}@0867d3c

// FUSE Relight RL-2.2: client IDirect3D9Ex and the d3d9.dll entry points.
//
// Semantics kept from upstream d3d9_module.cpp: adapter queries go to the host and are cached per
// argument set (adapter count, identifiers, mode counts, modes, display modes, caps); the device
// is created on the host with the client's present parameters; fullscreen windows are resized to
// the back buffer before creation (SetWindowMode). d3d9_bootstrap.cpp: Direct3DCreate9(Ex) create
// the client interface; the D3DPERF_* markers are accepted and ignored.
//
// Changes (revamp):
// - Exported directly by this d3d9.dll (no Detours over a system d3d9 loaded beside it).
// - The host process starts on the first Direct3DCreate9 call, not in DllMain (no process creation
//   under the loader lock).
// - Direct3DCreate9 fails (returns NULL, D3DERR_NOTAVAILABLE for the Ex form) when no host can be
//   started, instead of terminating the game.
// - CheckDeviceFormat results are cached too (games call it thousands of times at start-up).
// - DXVK's d3d8.dll runs on top of this d3d9.dll: IDxvkLegacyD3DInterfaceBridge /
//   IDxvkLegacyD3DDeviceBridge are implemented (dxvk_interop.cpp) so D3D8 games use the bridge.
#include "device.hpp"

#include <algorithm>
#include <cstring>

namespace fuse::relight::bridge::client {

namespace cmd = schema::cmd;

namespace {
uint64_t key(uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    return (uint64_t(a) << 48) ^ (uint64_t(b) << 24) ^ uint64_t(c) ^ (uint64_t(c) << 40);
}
}  // namespace

Interface::Interface(uint32_t h, bool ex) : BridgeObject(Kind::Interface, h, nullptr, nullptr), ex_(ex) {}

Interface::~Interface() {
    if (dxvkBridge_ != nullptr) {
        destroyDxvkInterfaceBridge(dxvkBridge_);
    }
}

HRESULT Interface::query(REFIID riid, void** ppv) {
    if (ppv != nullptr && isDxvkInterfaceBridgeIid(riid)) {
        FUSE_BRIDGE_LOCK();
        if (dxvkBridge_ == nullptr) {
            dxvkBridge_ = createDxvkInterfaceBridge(this);
        }
        if (dxvkBridge_ != nullptr) {
            dxvkBridge_->AddRef();
            *ppv = dxvkBridge_;
            return S_OK;
        }
    }
    if (ppv != nullptr && !ex_ && riid == __uuidof(IDirect3D9Ex)) {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    return queryCommon(this, static_cast<IDirect3D9Ex*>(this), riid, ppv, {&__uuidof(IDirect3D9), &__uuidof(IDirect3D9Ex)});
}

void Interface::destroyOnHost() { bridge().post(cmd::IDirect3D9Ex_Destroy {}, handle()); }

void Interface::setD3DCompatibility(uint32_t level) {
    FUSE_BRIDGE_LOCK();
    compatibility_ = level;
    cmd::IDirect3D9Ex_SetD3DCompatibility c;
    c.compatibility = level;
    bridge().call(c, handle(), D3DERR_INVALIDCALL);
}

HRESULT STDMETHODCALLTYPE Interface::RegisterSoftwareDevice(void*) { return D3D_OK; }  // DXVK: logged no-op

UINT STDMETHODCALLTYPE Interface::GetAdapterCount() {
    FUSE_BRIDGE_LOCK();
    if (adapterCount_ < 0) {
        cmd::Reply_Value r;
        adapterCount_ = bridge().query(cmd::IDirect3D9Ex_GetAdapterCount {}, handle(), r) ? int(r.value) : 0;
    }
    return UINT(adapterCount_);
}

HRESULT STDMETHODCALLTYPE Interface::GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id) {
    FUSE_BRIDGE_LOCK();
    if (id == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const uint64_t k = key(adapter, flags);
    auto it = identifiers_.find(k);
    if (it != identifiers_.end()) {
        *id = it->second;
        return D3D_OK;
    }
    cmd::IDirect3D9Ex_GetAdapterIdentifier c;
    c.adapter = adapter;
    c.flags = flags;
    cmd::Reply_AdapterIdentifier r;
    if (!bridge().query(c, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    if (FAILED(r.hresult)) {
        return HRESULT(r.hresult);
    }
    D3DADAPTER_IDENTIFIER9 out {};
    auto copyStr = [](char* dst, size_t cap, const std::string& s) {
        const size_t n = std::min(cap - 1, s.size());
        std::memcpy(dst, s.data(), n);
        dst[n] = '\0';
    };
    copyStr(out.Driver, sizeof(out.Driver), r.driver);
    copyStr(out.Description, sizeof(out.Description), r.description);
    copyStr(out.DeviceName, sizeof(out.DeviceName), r.deviceName);
    out.DriverVersion.QuadPart = LONGLONG(r.driverVersion);
    out.VendorId = r.vendorId;
    out.DeviceId = r.deviceId;
    out.SubSysId = r.subSysId;
    out.Revision = r.revision;
    std::memcpy(&out.DeviceIdentifier, r.deviceIdentifier.data(), sizeof(GUID));
    out.WHQLLevel = r.whqlLevel;
    identifiers_[k] = out;
    *id = out;
    return D3D_OK;
}

UINT STDMETHODCALLTYPE Interface::GetAdapterModeCount(UINT adapter, D3DFORMAT format) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3D9Ex_GetAdapterModeCount c;
    c.adapter = adapter;
    c.format = format;
    cmd::Reply_Value r;
    return bridge().query(c, handle(), r) ? r.value : 0;
}

HRESULT STDMETHODCALLTYPE Interface::EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* out) {
    FUSE_BRIDGE_LOCK();
    if (out == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3D9Ex_EnumAdapterModes c;
    c.adapter = adapter;
    c.format = format;
    c.mode = mode;
    cmd::Reply_DisplayMode r;
    if (!bridge().query(c, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    *out = D3DDISPLAYMODE {r.width, r.height, r.refreshRate, D3DFORMAT(r.format)};
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE Interface::GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) {
    FUSE_BRIDGE_LOCK();
    if (mode == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3D9Ex_GetAdapterDisplayMode c;
    c.adapter = adapter;
    cmd::Reply_DisplayMode r;
    if (!bridge().query(c, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    *mode = D3DDISPLAYMODE {r.width, r.height, r.refreshRate, D3DFORMAT(r.format)};
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE Interface::CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT displayFormat,
                                                     D3DFORMAT backBufferFormat, BOOL windowed) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3D9Ex_CheckDeviceType c;
    c.adapter = adapter;
    c.deviceType = type;
    c.adapterFormat = displayFormat;
    c.backBufferFormat = backBufferFormat;
    c.windowed = uint32_t(windowed);
    return bridge().call(c, handle(), D3DERR_NOTAVAILABLE);
}

HRESULT STDMETHODCALLTYPE Interface::CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage,
                                                       D3DRESOURCETYPE rtype, D3DFORMAT format) {
    FUSE_BRIDGE_LOCK();
    const uint64_t k = key(adapter * 16 + type, adapterFormat ^ (usage << 7) ^ (uint32_t(rtype) << 3), format);
    auto it = formatChecks_.find(k);
    if (it != formatChecks_.end()) {
        return it->second;
    }
    cmd::IDirect3D9Ex_CheckDeviceFormat c;
    c.adapter = adapter;
    c.deviceType = type;
    c.adapterFormat = adapterFormat;
    c.usage = usage;
    c.resourceType = rtype;
    c.checkFormat = format;
    const HRESULT hr = bridge().call(c, handle(), D3DERR_NOTAVAILABLE);
    if (bridge().alive()) {
        formatChecks_[k] = hr;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Interface::CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
                                                                D3DMULTISAMPLE_TYPE ms, DWORD* quality) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3D9Ex_CheckDeviceMultiSampleType c;
    c.adapter = adapter;
    c.deviceType = type;
    c.surfaceFormat = format;
    c.windowed = uint32_t(windowed);
    c.multiSampleType = ms;
    cmd::Reply_Value r;
    if (!bridge().query(c, handle(), r)) {
        return D3DERR_NOTAVAILABLE;
    }
    if (quality != nullptr) {
        *quality = r.value;
    }
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE Interface::CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                                            D3DFORMAT rtFormat, D3DFORMAT dsFormat) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3D9Ex_CheckDepthStencilMatch c;
    c.adapter = adapter;
    c.deviceType = type;
    c.adapterFormat = adapterFormat;
    c.renderTargetFormat = rtFormat;
    c.depthStencilFormat = dsFormat;
    return bridge().call(c, handle(), D3DERR_NOTAVAILABLE);
}

HRESULT STDMETHODCALLTYPE Interface::CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT src, D3DFORMAT dst) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3D9Ex_CheckDeviceFormatConversion c;
    c.adapter = adapter;
    c.deviceType = type;
    c.sourceFormat = src;
    c.targetFormat = dst;
    return bridge().call(c, handle(), D3DERR_NOTAVAILABLE);
}

HRESULT STDMETHODCALLTYPE Interface::GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) {
    FUSE_BRIDGE_LOCK();
    if (caps == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const uint64_t k = key(adapter, type);
    auto it = caps_.find(k);
    if (it != caps_.end()) {
        *caps = it->second;
        return D3D_OK;
    }
    cmd::IDirect3D9Ex_GetDeviceCaps c;
    c.adapter = adapter;
    c.deviceType = type;
    cmd::Reply_Caps r;
    if (!bridge().query(c, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    if (FAILED(r.hresult)) {
        return HRESULT(r.hresult);
    }
    caps_[k] = wire::capsFromWire(r.caps);
    *caps = caps_[k];
    return D3D_OK;
}

HMONITOR STDMETHODCALLTYPE Interface::GetAdapterMonitor(UINT adapter) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3D9Ex_GetAdapterMonitor c;
    c.adapter = adapter;
    cmd::Reply_Value r;
    if (!bridge().query(c, handle(), r)) {
        return nullptr;
    }
    // HMONITORs are process-independent handles; the host reports its own.
    return wire::handleFromWire<HMONITOR>(r.value);
}

HRESULT Interface::createDevice(bool ex, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                D3DDISPLAYMODEEX* mode, Device** out) {
    FUSE_BRIDGE_LOCK();
    if (pp == nullptr || out == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *out = nullptr;
    D3DCAPS9 caps;
    HRESULT hr = GetDeviceCaps(adapter, type, &caps);
    if (FAILED(hr)) {
        return hr;
    }
    D3DPRESENT_PARAMETERS local = *pp;
    if (local.BackBufferCount == 0) {
        local.BackBufferCount = 1;
    }
    HWND window = local.hDeviceWindow ? local.hDeviceWindow : focus;
    if ((local.BackBufferWidth == 0 || local.BackBufferHeight == 0) && window != nullptr) {
        RECT r {};
        ::GetClientRect(window, &r);
        if (local.BackBufferWidth == 0) {
            local.BackBufferWidth = UINT(std::max<LONG>(r.right - r.left, 1));
        }
        if (local.BackBufferHeight == 0) {
            local.BackBufferHeight = UINT(std::max<LONG>(r.bottom - r.top, 1));
        }
    }
    if (local.BackBufferFormat == D3DFMT_UNKNOWN && local.Windowed) {
        local.BackBufferFormat = D3DFMT_X8R8G8B8;
    }
    D3DDEVICE_CREATION_PARAMETERS cp {adapter, type, focus, flags};
    auto* dev = new Device(newHandle(), this, ex, cp, caps);
    if (ex) {
        cmd::IDirect3D9Ex_CreateDeviceEx c;
        c.adapter = adapter;
        c.deviceType = type;
        c.focusWindow = wire::handleToWire(focus);
        c.behaviorFlags = flags;
        c.presentParameters = wire::toWire(*pp);
        c.fullscreenDisplayMode = wire::displayModeExToWire(mode);
        c.result = dev->handle();
        hr = bridge().call(c, handle(), D3DERR_NOTAVAILABLE);
    } else {
        cmd::IDirect3D9Ex_CreateDevice c;
        c.adapter = adapter;
        c.deviceType = type;
        c.focusWindow = wire::handleToWire(focus);
        c.behaviorFlags = flags;
        c.presentParameters = wire::toWire(*pp);
        c.result = dev->handle();
        hr = bridge().call(c, handle(), D3DERR_NOTAVAILABLE);
    }
    if (FAILED(hr)) {
        dev->addRef();
        dev->release();  // host has no twin; the Destroy it sends is ignored there
        return hr;
    }
    dev->addRef();
    dev->initImplicit(local);
    *out = dev;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Interface::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                                  D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** device) {
    Device* d = nullptr;
    const HRESULT hr = createDevice(false, adapter, type, focus, flags, pp, nullptr, &d);
    if (device != nullptr) {
        *device = d;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Interface::CreateDeviceEx(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                                    D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode,
                                                    IDirect3DDevice9Ex** device) {
    Device* d = nullptr;
    const HRESULT hr = createDevice(true, adapter, type, focus, flags, pp, mode, &d);
    if (device != nullptr) {
        *device = d;
    }
    return hr;
}

UINT STDMETHODCALLTYPE Interface::GetAdapterModeCountEx(UINT adapter, const D3DDISPLAYMODEFILTER* filter) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3D9Ex_GetAdapterModeCountEx c;
    c.adapter = adapter;
    c.filter = wire::filterToWire(filter);
    cmd::Reply_Value r;
    return bridge().query(c, handle(), r) ? r.value : 0;
}

HRESULT STDMETHODCALLTYPE Interface::EnumAdapterModesEx(UINT adapter, const D3DDISPLAYMODEFILTER* filter, UINT mode,
                                                        D3DDISPLAYMODEEX* out) {
    FUSE_BRIDGE_LOCK();
    if (out == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3D9Ex_EnumAdapterModesEx c;
    c.adapter = adapter;
    c.filter = wire::filterToWire(filter);
    c.mode = mode;
    cmd::Reply_DisplayMode r;
    if (!bridge().query(c, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    *out = D3DDISPLAYMODEEX {sizeof(D3DDISPLAYMODEEX), r.width, r.height, r.refreshRate, D3DFORMAT(r.format),
                             D3DSCANLINEORDERING(r.scanLineOrdering)};
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE Interface::GetAdapterDisplayModeEx(UINT adapter, D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3D9Ex_GetAdapterDisplayModeEx c;
    c.adapter = adapter;
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

HRESULT STDMETHODCALLTYPE Interface::GetAdapterLUID(UINT adapter, LUID* luid) {
    FUSE_BRIDGE_LOCK();
    if (luid == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3D9Ex_GetAdapterLUID c;
    c.adapter = adapter;
    cmd::Reply_Luid r;
    if (!bridge().query(c, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    luid->LowPart = r.lowPart;
    luid->HighPart = r.highPart;
    return HRESULT(r.hresult);
}

// ---- entry points ------------------------------------------------------------------------------------------------

Interface* createInterface(bool ex) {
    FUSE_BRIDGE_LOCK();
    if (!bridge().start()) {
        return nullptr;
    }
    auto* itf = new Interface(newHandle(), ex);
    cmd::Direct3DCreate9 c;
    c.sdkVersion = D3D_SDK_VERSION;
    c.ex = ex ? 1 : 0;
    c.result = itf->handle();
    if (FAILED(bridge().call(c, 0, E_FAIL))) {
        delete itf;
        return nullptr;
    }
    itf->addRef();
    return itf;
}

}  // namespace fuse::relight::bridge::client

using fuse::relight::bridge::client::createInterface;

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT) { return createInterface(false); }

HRESULT WINAPI Direct3DCreate9Ex(UINT, IDirect3D9Ex** out) {
    if (out == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *out = createInterface(true);
    return *out ? D3D_OK : D3DERR_NOTAVAILABLE;
}

// D3D9On12 is not bridged: create a plain interface (DXVK does the same when 9On12 is disabled).
IDirect3D9* WINAPI Direct3DCreate9On12(UINT sdk, void*, UINT) { return Direct3DCreate9(sdk); }
HRESULT WINAPI Direct3DCreate9On12Ex(UINT sdk, void*, UINT, IDirect3D9Ex** out) {
    return Direct3DCreate9Ex(sdk, out);
}

void* WINAPI Direct3DShaderValidatorCreate9() { return nullptr; }
int WINAPI D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR) { return 0; }
int WINAPI D3DPERF_EndEvent() { return 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR, LPCWSTR) {}
void WINAPI D3DPERF_SetRegion(D3DCOLOR, LPCWSTR) {}
BOOL WINAPI D3DPERF_QueryRepeatFrame() { return FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD) {}
DWORD WINAPI D3DPERF_GetStatus() { return 0; }
void WINAPI DebugSetMute() {}
int WINAPI DebugSetLevel() { return 0; }
void WINAPI PSGPError() {}
void WINAPI PSGPSampleTexture() {}
void WINAPI Direct3D9EnableMaximizedWindowedModeShim(UINT) {}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        // Unload or process exit: terminate the host session cleanly (it would otherwise notice the
        // client's death through its peer supervision).
        fuse::relight::bridge::client::bridge().shutdown();
    }
    return TRUE;
}

}  // extern "C"
