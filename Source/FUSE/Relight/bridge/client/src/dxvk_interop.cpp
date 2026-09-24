// FUSE Relight RL-2.2: DXVK d3d8 interop of the bridge client.
// Copyright (c) 2026 FUSE contributors (MIT). New code (upstream bridge had no D3D8 path; Remix
// packaging used crosire's d3d8to9). The interface declarations come from the vendored DXVK
// src/d3d9/d3d9_bridge.h (zlib), unchanged.
//
// DXVK's d3d8.dll wraps "d3d9.dll" (it imports Direct3DCreate9 by name) and requires the DXVK
// interop interfaces IDxvkLegacyD3DInterfaceBridge / IDxvkLegacyD3DDeviceBridge on the objects it
// gets back. Placed next to this client, a DXVK d3d8.dll therefore runs on top of the bridge:
// SetD3DCompatibility goes to the host (the host's DXVK then applies its D3D8 validation rules),
// UpdateTextureFromBuffer / IsSupportedSurfaceFormat are bridge commands, and GetConfig returns the
// client-side DXVK configuration (dxvk.conf + DXVK_CONFIG + per-app defaults), read with DXVK's own
// config parser so the d3d8 options behave as they do in-process.
#include "device.hpp"

#include <atomic>

#include "d3d9/d3d9_include.h"
#include "d3d9/d3d9_bridge.h"
#include "util/config/config.h"
#include "util/util_env.h"

// DXVK's util library logs through this per-module instance (d3d9_main.cpp defines it in DXVK's own
// d3d9.dll); DXVK_LOG_PATH / DXVK_LOG_LEVEL apply as usual.
dxvk::Logger dxvk::Logger::s_instance("d3d9_bridge.log");

namespace fuse::relight::bridge::client {

namespace {

const dxvk::Config* clientConfig() {
    static const dxvk::Config* config = [] {
        auto* c = new dxvk::Config(dxvk::Config::getUserConfig());
        c->merge(dxvk::Config::getAppConfig(dxvk::env::getExePath()));
        return c;
    }();
    return config;
}

class InterfaceBridge final : public IDxvkLegacyD3DInterfaceBridge {
public:
    explicit InterfaceBridge(Interface* owner) : owner_(owner) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return owner_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return owner_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override { return owner_->QueryInterface(riid, ppv); }
    void SetD3DCompatibility(DxvkD3DCompatibility level) const override { owner_->setD3DCompatibility(uint32_t(level)); }
    const dxvk::Config* GetConfig() const override { return clientConfig(); }

private:
    Interface* owner_;
};

class DeviceBridge final : public IDxvkLegacyD3DDeviceBridge {
public:
    explicit DeviceBridge(Device* owner) : owner_(owner) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return owner_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return owner_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override { return owner_->QueryInterface(riid, ppv); }
    HRESULT UpdateTextureFromBuffer(IDirect3DSurface9* dst, IDirect3DSurface9* src, const RECT* srcRect,
                                    const POINT* dstPoint) override {
        return owner_->updateTextureFromBuffer(dst, src, srcRect, dstPoint);
    }
    bool IsSupportedSurfaceFormat(D3DFORMAT format) override { return owner_->isSupportedSurfaceFormat(format); }

private:
    Device* owner_;
};

}  // namespace

// The wrappers' AddRef/Release forward to their owner (the owner's refcount is the only one); the
// owner creates its wrapper on the first QueryInterface and destroys it in its own teardown.
IUnknown* createDxvkInterfaceBridge(Interface* owner) { return new InterfaceBridge(owner); }
IUnknown* createDxvkDeviceBridge(Device* owner) { return new DeviceBridge(owner); }
void destroyDxvkInterfaceBridge(IUnknown* p) { delete static_cast<InterfaceBridge*>(static_cast<IDxvkLegacyD3DInterfaceBridge*>(p)); }
void destroyDxvkDeviceBridge(IUnknown* p) { delete static_cast<DeviceBridge*>(static_cast<IDxvkLegacyD3DDeviceBridge*>(p)); }
bool isDxvkInterfaceBridgeIid(REFIID riid) { return riid == __uuidof(IDxvkLegacyD3DInterfaceBridge); }
bool isDxvkDeviceBridgeIid(REFIID riid) { return riid == __uuidof(IDxvkLegacyD3DDeviceBridge); }

}  // namespace fuse::relight::bridge::client
