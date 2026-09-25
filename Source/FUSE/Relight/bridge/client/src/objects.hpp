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
// Ported from dxvk-remix bridge/src/client/{base.h,d3d9_lss.h,d3d9_device.h,d3d9_device_base.h,
// d3d9_resource.h,d3d9_base_texture.h,d3d9_texture.h,d3d9_cubetexture.h,d3d9_volumetexture.h,
// d3d9_surface.h,d3d9_volume.h,d3d9_vertexbuffer.h,d3d9_indexbuffer.h,lockable_buffer.h,
// d3d9_swapchain.h,d3d9_stateblock.h,d3d9_query.h,d3d9_vertexshader.h,d3d9_pixelshader.h,
// d3d9_vertexdeclaration.h,d3d9_privatedata.h,d3d9_commonshader.h}@0867d3c

// FUSE Relight RL-2.2: the client's D3D9 COM objects. Every object has a client-chosen handle that
// names its host-side twin; state and resource contents the game can read back live here too.
//
// Semantics kept from upstream:
// - Refcounts (base.h D3DRefCounted/D3DBase): a public (interface) count the game sees and a private
//   count for internal references (bound state); the object dies when both are zero, and only then
//   is its host twin destroyed. Container children (texture levels, cube faces, volume levels, back
//   buffers) share their container's refcount and are unlinked on the host when it dies.
// - A standalone device child holds a public device reference while the game holds it (so
//   IDirect3DDevice9::Release returns the D3D9 count).
// - Resource contents are shadowed: Lock hands out client memory; Unlock ships the locked region.
//   Private data stays in the client.
//
// Changes (revamp):
// - Object identity is a private COM interface (kIidBridgeObject) instead of the global
//   pointer -> object shadow map (gShadowMap) and bridge_cast; foreign objects are rejected.
// - Surfaces, cube faces and texture levels share one Subresource per level, so a texture LockRect
//   and a GetSurfaceLevel()->LockRect see the same bytes (upstream kept separate paths).
// - Read-back: resources the host may have written (render targets, depth, back buffers, and the
//   destinations of GetRenderTargetData / StretchRect / ColorFill / UpdateSurface / UpdateTexture /
//   ProcessVertices / GenerateMipSubLevels) are "host dirty"; a non-DISCARD lock of a host-dirty
//   subresource fetches the locked region from the host (Reply_Data). Upstream only read back the
//   back buffer (client.enableBackbufferCapture) and GetRenderTargetData destinations and otherwise
//   returned stale client memory.
// - Uploads send only the locked region, tightly packed, inline or through the shared heap.
#pragma once

#include "connection.hpp"
#include "device_state.hpp"

#include <fuse/relight/bridge/client/format_layout.hpp>
#include <fuse/relight/bridge/client/wire_types.hpp>

#include <d3d9.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace fuse::relight::bridge::client {

class Device;
class Interface;
class Surface;
class Volume;

// {5A4F1B0E-7C2D-4E83-9F61-0D3B8E2A7C15}: QueryInterface(kIidBridgeObject) yields the BridgeObject*
// (no AddRef). Only the client's own objects answer it.
extern const GUID kIidBridgeObject;

enum class Kind : uint8_t {
    Interface, Device, SwapChain, StateBlock, VertexDeclaration, VertexShader, PixelShader, Query,
    Texture, VolumeTexture, CubeTexture, VertexBuffer, IndexBuffer, Surface, Volume,
};

class BridgeObject {
public:
    BridgeObject(Kind kind, uint32_t handle, Device* device, BridgeObject* container);
    virtual ~BridgeObject() = default;
    BridgeObject(const BridgeObject&) = delete;
    BridgeObject& operator=(const BridgeObject&) = delete;

    Kind kind() const noexcept { return kind_; }
    uint32_t handle() const noexcept { return handle_; }
    Device* device() const noexcept { return device_; }
    BridgeObject* container() const noexcept { return container_; }

    ULONG addRef();
    ULONG release();
    void addRefPrivate();
    void releasePrivate();
    ULONG publicRefs() const noexcept { return pub_.load(); }
    // The IUnknown of the object's main interface (for GetContainer / QueryInterface forwarding).
    virtual IUnknown* unknown() = 0;
    // Container teardown of a child (texture level, back buffer): unlink on the host and delete.
    void teardownChild() {
        destroyOnHost();
        delete this;
    }

protected:
    // Destroys (or unlinks) the host twin; called once, right before deletion.
    virtual void destroyOnHost() = 0;
    // Final teardown; containers override to delete their children first.
    virtual void finalRelease();
    bool holdsDevice_ = true;

private:
    void maybeDestroy();

    Kind kind_;
    uint32_t handle_;
    Device* device_;
    BridgeObject* container_;
    std::atomic<ULONG> pub_ {0};
    std::atomic<ULONG> priv_ {0};
    bool destroyed_ = false;
};

// Returns the client object behind a COM pointer of the game (nullptr for foreign or null objects).
BridgeObject* bridgeObject(IUnknown* p);
template <class T>
T* bridgeCast(IUnknown* p) {
    return static_cast<T*>(bridgeObject(p));
}
inline uint32_t handleOf(IUnknown* p) {
    BridgeObject* o = bridgeObject(p);
    return o ? o->handle() : 0;
}

#define FUSE_BRIDGE_IUNKNOWN                                                                      \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override { return query(riid, ppv); } \
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }                               \
    ULONG STDMETHODCALLTYPE Release() override { return release(); }

// Answers IUnknown + the given interfaces + kIidBridgeObject.
HRESULT queryCommon(BridgeObject* self, IUnknown* iface, REFIID riid, void** ppv, std::initializer_list<const GUID*> iids);

// ---- private data (d3d9_privatedata.h) -----------------------------------------------------------
class PrivateData {
public:
    ~PrivateData() { clear(); }
    HRESULT set(REFGUID guid, const void* data, DWORD size, DWORD flags);
    HRESULT get(REFGUID guid, void* data, DWORD* size);
    HRESULT free(REFGUID guid);
    void clear();

private:
    struct Entry {
        std::vector<uint8_t> bytes;
        IUnknown* unknown = nullptr;
    };
    std::map<GUID, Entry, bool (*)(const GUID&, const GUID&)> entries_ {
        [](const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) < 0; }};
};

// ---- pixel storage of one surface / volume level ---------------------------------------------------
struct Subresource {
    uint32_t width = 1, height = 1, depth = 1;
    uint32_t format = 0;
    FormatLayout layout;
    std::vector<uint8_t> shadow;
    bool hostDirty = false;    // the host may hold newer contents than `shadow`
    bool gpuWritable = false;  // render target / depth / back buffer: every lock reads back
    struct LockRecord {
        std::vector<int32_t> wireRegion;  // as the game passed it (empty = whole)
        RegionLayout rl;
        DWORD flags = 0;
    };
    std::deque<LockRecord> locks;

    void init(uint32_t w, uint32_t h, uint32_t d, uint32_t fmt);
    uint8_t* storage();
    bool needsReadback(DWORD flags) const noexcept {
        return (gpuWritable || hostDirty) && !(flags & D3DLOCK_DISCARD);
    }
};

// Region read / upload callbacks: the owner knows which command addresses the subresource.
using ReadFn = std::function<HRESULT(const std::vector<int32_t>& wireRegion, std::vector<uint8_t>& packed)>;
using UploadFn = std::function<void(const std::vector<int32_t>& wireRegion, DWORD flags, const RegionLayout& rl,
                                    const std::vector<uint8_t>& packed)>;

HRESULT lockSubresource(Subresource& s, const std::vector<int32_t>& wireRegion, DWORD flags, void** bits,
                        INT* rowPitch, INT* slicePitch, const ReadFn& read);
HRESULT unlockSubresource(Subresource& s, const UploadFn& upload);
// Stores a whole-subresource read-back (GetRenderTargetData / GetFrontBufferData Reply_Data).
void fillSubresource(Subresource& s, const std::vector<uint8_t>& packed);
// Sends a whole-surface read-back command and stores the result in `dst` (Reply_Data).
template <class Cmd>
HRESULT readWholeSurface(const Cmd& c, uint32_t target, Surface* dst);

// ---- resource base (d3d9_resource.h) -------------------------------------------------------------
template <class Iface>
class ResourceBase : public Iface, public BridgeObject {
public:
    ResourceBase(Kind k, uint32_t h, Device* dev, BridgeObject* container, D3DPOOL pool)
        : BridgeObject(k, h, dev, container), pool_(pool) {}
    IUnknown* unknown() override { return static_cast<Iface*>(this); }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override {
        FUSE_BRIDGE_LOCK();
        return privateData_.set(guid, data, size, flags);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) override {
        FUSE_BRIDGE_LOCK();
        return privateData_.get(guid, data, size);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override {
        FUSE_BRIDGE_LOCK();
        return privateData_.free(guid);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override;
    DWORD STDMETHODCALLTYPE GetPriority() override { return priority_; }
    void STDMETHODCALLTYPE PreLoad() override;

    D3DPOOL pool() const noexcept { return pool_; }

protected:
    D3DPOOL pool_;
    DWORD priority_ = 0;
    PrivateData privateData_;
};

// ---- surfaces / volumes (d3d9_surface.h, d3d9_volume.h) ------------------------------------------
class Surface;
class Surface final : public ResourceBase<IDirect3DSurface9> {
public:
    // Standalone surface (owns its storage) or container child (face/level of a texture, back buffer).
    Surface(uint32_t handle, Device* dev, const D3DSURFACE_DESC& desc, BridgeObject* container, Subresource* shared);
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** ppContainer) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect() override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC* phdc) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override;

    const D3DSURFACE_DESC& desc() const noexcept { return desc_; }
    Subresource& sub() noexcept { return *sub_; }
    void markHostDirty() noexcept { sub_->hostDirty = true; }
    void markGpuWritable() noexcept { sub_->gpuWritable = true; }

protected:
    void destroyOnHost() override;

private:
    D3DSURFACE_DESC desc_;
    Subresource own_;
    Subresource* sub_;
    struct DcState {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        void* bits = nullptr;
    } dc_;
};

template <class Cmd>
HRESULT readWholeSurface(const Cmd& c, uint32_t target, Surface* dst) {
    std::vector<uint8_t> packed;
    const HRESULT hr = bridge().readData(c, target, packed, D3DERR_DEVICELOST);
    if (SUCCEEDED(hr)) {
        fillSubresource(dst->sub(), packed);
    }
    return hr;
}

class Volume final : public IDirect3DVolume9, public BridgeObject {
public:
    Volume(uint32_t handle, Device* dev, const D3DVOLUME_DESC& desc, BridgeObject* container, Subresource* shared);
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    IUnknown* unknown() override { return static_cast<IDirect3DVolume9*>(this); }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override;
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** ppContainer) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockBox() override;

    Subresource& sub() noexcept { return *sub_; }

protected:
    void destroyOnHost() override;

private:
    D3DVOLUME_DESC desc_;
    Subresource* sub_;
    PrivateData privateData_;
};

// ---- textures (d3d9_base_texture.h, d3d9_texture.h, d3d9_cubetexture.h, d3d9_volumetexture.h) -----
struct TextureDesc {
    UINT width = 1, height = 1, depth = 1, levels = 1;
    DWORD usage = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    D3DPOOL pool = D3DPOOL_DEFAULT;
};

template <class Iface>
class TextureBase : public ResourceBase<Iface> {
public:
    TextureBase(Kind k, uint32_t h, Device* dev, const TextureDesc& desc, uint32_t faces);
    DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) override;
    DWORD STDMETHODCALLTYPE GetLOD() override { return lod_; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return desc_.levels; }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE filter) override;
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return filter_; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() override;

    const TextureDesc& desc() const noexcept { return desc_; }
    Subresource& sub(uint32_t face, uint32_t level) { return subs_[face * desc_.levels + level]; }
    uint32_t faces() const noexcept { return faces_; }
    // Every level may have been written by the host (UpdateTexture destination, render target).
    void markHostDirty(bool fromLevel1 = false) noexcept;
    void markGpuWritable() noexcept;

protected:
    void destroyOnHost() override;
    void finalRelease() override;
    // Children created on demand (GetSurfaceLevel / GetCubeMapSurface / GetVolumeLevel).
    std::vector<BridgeObject*> children_;
    TextureDesc desc_;
    uint32_t faces_;
    std::vector<Subresource> subs_;
    DWORD lod_ = 0;
    D3DTEXTUREFILTERTYPE filter_ = D3DTEXF_LINEAR;
};

class Texture final : public TextureBase<IDirect3DTexture9> {
public:
    Texture(uint32_t h, Device* dev, const TextureDesc& desc);
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface9** ppSurface) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT* rect) override;
    D3DSURFACE_DESC levelDesc(UINT level) const;
    Surface* level(UINT level);  // creates + links the child
};

class CubeTexture final : public TextureBase<IDirect3DCubeTexture9> {
public:
    CubeTexture(uint32_t h, Device* dev, const TextureDesc& desc);
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_CUBETEXTURE; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9** ppSurface) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect,
                                       DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES face, const RECT* rect) override;
    D3DSURFACE_DESC levelDesc(UINT level) const;
};

class VolumeTexture final : public TextureBase<IDirect3DVolumeTexture9> {
public:
    VolumeTexture(uint32_t h, Device* dev, const TextureDesc& desc);
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DVOLUME_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT level, IDirect3DVolume9** ppVolume) override;
    HRESULT STDMETHODCALLTYPE LockBox(UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX* box) override;
    D3DVOLUME_DESC levelDesc(UINT level) const;
};

// ---- vertex / index buffers (lockable_buffer.h) --------------------------------------------------
template <class Iface, class Desc>
class Buffer : public ResourceBase<Iface> {
public:
    Buffer(Kind k, uint32_t h, Device* dev, const Desc& desc);
    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, void** ppData, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Unlock() override;
    HRESULT STDMETHODCALLTYPE GetDesc(Desc* pDesc) override;

    const Desc& desc() const noexcept { return desc_; }
    void markHostDirty() noexcept { hostDirty_ = true; }

protected:
    void destroyOnHost() override;

private:
    Desc desc_;
    std::vector<uint8_t> shadow_;
    bool hostDirty_ = false;
    struct LockRecord {
        UINT offset, size;
        DWORD flags;
    };
    std::deque<LockRecord> locks_;
};

class VertexBuffer final : public Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC> {
public:
    VertexBuffer(uint32_t h, Device* dev, const D3DVERTEXBUFFER_DESC& desc)
        : Buffer(Kind::VertexBuffer, h, dev, desc) {}
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
};

class IndexBuffer final : public Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC> {
public:
    IndexBuffer(uint32_t h, Device* dev, const D3DINDEXBUFFER_DESC& desc) : Buffer(Kind::IndexBuffer, h, dev, desc) {}
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }
};

// ---- swap chain (d3d9_swapchain.h) ---------------------------------------------------------------
class SwapChain final : public IDirect3DSwapChain9Ex, public BridgeObject {
public:
    SwapChain(uint32_t h, Device* dev, const D3DPRESENT_PARAMETERS& pp, uint32_t implicitIndex);
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    IUnknown* unknown() override { return static_cast<IDirect3DSwapChain9Ex*>(this); }
    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty,
                                      DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9* dst) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** ppBackBuffer) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* status) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* mode) override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* pp) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override;
    HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS* stats) override;
    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) override;

    Surface* backBuffer(UINT index);  // creates + links
    const D3DPRESENT_PARAMETERS& params() const noexcept { return pp_; }

protected:
    void destroyOnHost() override;
    void finalRelease() override;

private:
    D3DPRESENT_PARAMETERS pp_;
    uint32_t implicitIndex_;
    std::vector<Surface*> backBuffers_;
};

// ---- small objects (d3d9_stateblock.h, d3d9_query.h, shaders, vertex declaration) ----------------
class StateBlock final : public IDirect3DStateBlock9, public BridgeObject {
public:
    StateBlock(uint32_t h, Device* dev, bool softwareVp);
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    IUnknown* unknown() override { return static_cast<IDirect3DStateBlock9*>(this); }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE Capture() override;
    HRESULT STDMETHODCALLTYPE Apply() override;

    DeviceState state;
    StateMask mask;

protected:
    void destroyOnHost() override;
};

class VertexDeclaration final : public IDirect3DVertexDeclaration9, public BridgeObject {
public:
    VertexDeclaration(uint32_t h, Device* dev, std::vector<D3DVERTEXELEMENT9> elements);
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    IUnknown* unknown() override { return static_cast<IDirect3DVertexDeclaration9*>(this); }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9* elements, UINT* count) override;

protected:
    void destroyOnHost() override;

private:
    std::vector<D3DVERTEXELEMENT9> elements_;  // D3DDECL_END included
};

template <class Iface, Kind K>
class Shader final : public Iface, public BridgeObject {
public:
    Shader(uint32_t h, Device* dev, std::vector<DWORD> code) : BridgeObject(K, h, dev, nullptr), code_(std::move(code)) {}
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    IUnknown* unknown() override { return static_cast<Iface*>(this); }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetFunction(void* data, UINT* size) override;

protected:
    void destroyOnHost() override;

private:
    std::vector<DWORD> code_;
};
using VertexShader = Shader<IDirect3DVertexShader9, Kind::VertexShader>;
using PixelShader = Shader<IDirect3DPixelShader9, Kind::PixelShader>;

class Query final : public IDirect3DQuery9, public BridgeObject {
public:
    Query(uint32_t h, Device* dev, D3DQUERYTYPE type);
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    IUnknown* unknown() override { return static_cast<IDirect3DQuery9*>(this); }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return type_; }
    DWORD STDMETHODCALLTYPE GetDataSize() override;
    HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override;
    HRESULT STDMETHODCALLTYPE GetData(void* data, DWORD size, DWORD flags) override;
    static DWORD dataSize(D3DQUERYTYPE type);

protected:
    void destroyOnHost() override;

private:
    D3DQUERYTYPE type_;
};

// ---- IDirect3D9Ex (d3d9_module.cpp) --------------------------------------------------------------
class Interface final : public IDirect3D9Ex, public BridgeObject {
public:
    Interface(uint32_t h, bool ex);
    ~Interface() override;
    FUSE_BRIDGE_IUNKNOWN
    HRESULT query(REFIID riid, void** ppv);
    IUnknown* unknown() override { return static_cast<IDirect3D9Ex*>(this); }

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* init) override;
    UINT STDMETHODCALLTYPE GetAdapterCount() override;
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id) override;
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter, D3DFORMAT format) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* out) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT displayFormat,
                                              D3DFORMAT backBufferFormat, BOOL windowed) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage,
                                                D3DRESOURCETYPE rtype, D3DFORMAT format) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
                                                         D3DMULTISAMPLE_TYPE ms, DWORD* quality) override;
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                                     D3DFORMAT rtFormat, D3DFORMAT dsFormat) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT src,
                                                          D3DFORMAT dst) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) override;
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override;
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** device) override;
    UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT adapter, const D3DDISPLAYMODEFILTER* filter) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT adapter, const D3DDISPLAYMODEFILTER* filter, UINT mode,
                                                 D3DDISPLAYMODEEX* out) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT adapter, D3DDISPLAYMODEEX* mode,
                                                      D3DDISPLAYROTATION* rotation) override;
    HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                             D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode,
                                             IDirect3DDevice9Ex** device) override;
    HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT adapter, LUID* luid) override;

    bool isEx() const noexcept { return ex_; }
    // IDxvkLegacyD3DInterfaceBridge (d3d8): the compatibility level forwarded to the host.
    void setD3DCompatibility(uint32_t level);
    uint32_t d3dCompatibility() const noexcept { return compatibility_; }

protected:
    void destroyOnHost() override;

private:
    HRESULT createDevice(bool ex, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                         D3DDISPLAYMODEEX* mode, Device** out);
    bool ex_;
    uint32_t compatibility_ = 0;
    int adapterCount_ = -1;
    std::map<uint64_t, D3DCAPS9> caps_;
    std::map<uint64_t, D3DADAPTER_IDENTIFIER9> identifiers_;
    std::map<uint64_t, HRESULT> formatChecks_;
    IUnknown* dxvkBridge_ = nullptr;  // IDxvkLegacyD3DInterfaceBridge (dxvk_interop.cpp)
};

// Created in dxvk_interop.cpp when the DXVK interop is compiled in (else nullptr).
IUnknown* createDxvkInterfaceBridge(Interface* owner);
IUnknown* createDxvkDeviceBridge(Device* owner);
void destroyDxvkInterfaceBridge(IUnknown* bridge);
void destroyDxvkDeviceBridge(IUnknown* bridge);
bool isDxvkInterfaceBridgeIid(REFIID riid);
bool isDxvkDeviceBridgeIid(REFIID riid);

// The client's handle allocator.
inline uint32_t newHandle() { return bridge().newHandle(); }

}  // namespace fuse::relight::bridge::client
