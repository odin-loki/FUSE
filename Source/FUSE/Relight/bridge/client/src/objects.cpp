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
// Ported from dxvk-remix bridge/src/client/{base.h,d3d9_privatedata.h,d3d9_surface.cpp,d3d9_volume.cpp,
// d3d9_texture.cpp,d3d9_cubetexture.cpp,d3d9_volumetexture.cpp,lockable_buffer.h,d3d9_swapchain.cpp,
// d3d9_stateblock.cpp,d3d9_query.cpp,d3d9_vertexshader.cpp,d3d9_pixelshader.cpp,
// d3d9_vertexdeclaration.cpp}@0867d3c

// FUSE Relight RL-2.2: client COM objects other than IDirect3D9Ex / IDirect3DDevice9Ex (objects.hpp).
#include "device.hpp"

#include <algorithm>
#include <cstring>

namespace fuse::relight::bridge::client {

namespace cmd = schema::cmd;

const GUID kIidBridgeObject = {0x5a4f1b0e, 0x7c2d, 0x4e83, {0x9f, 0x61, 0x0d, 0x3b, 0x8e, 0x2a, 0x7c, 0x15}};

// ---- BridgeObject ---------------------------------------------------------------------------------

BridgeObject::BridgeObject(Kind kind, uint32_t handle, Device* device, BridgeObject* container)
    : kind_(kind), handle_(handle), device_(device), container_(container) {
    if (container != nullptr || device == nullptr) {
        holdsDevice_ = false;
    }
}

ULONG BridgeObject::addRef() {
    if (container_ != nullptr) {
        return container_->addRef();
    }
    FUSE_BRIDGE_LOCK();
    const ULONG n = ++pub_;
    if (n == 1 && holdsDevice_) {
        device_->addRef();
    }
    return n;
}

ULONG BridgeObject::release() {
    if (container_ != nullptr) {
        return container_->release();
    }
    FUSE_BRIDGE_LOCK();
    if (pub_.load() == 0) {
        return 0;
    }
    const ULONG n = --pub_;
    if (n == 0) {
        Device* dev = holdsDevice_ ? device_ : nullptr;
        maybeDestroy();  // may delete this
        if (dev != nullptr) {
            dev->release();
        }
    }
    return n;
}

void BridgeObject::addRefPrivate() {
    if (container_ != nullptr) {
        container_->addRefPrivate();
        return;
    }
    FUSE_BRIDGE_LOCK();
    ++priv_;
}

void BridgeObject::releasePrivate() {
    if (container_ != nullptr) {
        container_->releasePrivate();
        return;
    }
    FUSE_BRIDGE_LOCK();
    if (priv_.load() == 0) {
        return;
    }
    if (--priv_ == 0) {
        maybeDestroy();
    }
}

void BridgeObject::maybeDestroy() {
    if (pub_.load() == 0 && priv_.load() == 0 && !destroyed_) {
        destroyed_ = true;
        finalRelease();
    }
}

void BridgeObject::finalRelease() {
    destroyOnHost();
    delete this;
}

BridgeObject* bridgeObject(IUnknown* p) {
    if (p == nullptr) {
        return nullptr;
    }
    void* o = nullptr;
    if (FAILED(p->QueryInterface(kIidBridgeObject, &o)) || o == nullptr) {
        return nullptr;
    }
    return static_cast<BridgeObject*>(o);
}

HRESULT queryCommon(BridgeObject* self, IUnknown* iface, REFIID riid, void** ppv,
                    std::initializer_list<const GUID*> iids) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (riid == kIidBridgeObject) {
        *ppv = static_cast<void*>(self);
        return S_OK;
    }
    bool match = riid == __uuidof(IUnknown);
    for (const GUID* g : iids) {
        match = match || riid == *g;
    }
    if (!match) {
        return E_NOINTERFACE;
    }
    *ppv = iface;
    self->addRef();
    return S_OK;
}

// ---- PrivateData ----------------------------------------------------------------------------------

HRESULT PrivateData::set(REFGUID guid, const void* data, DWORD size, DWORD flags) {
    if (data == nullptr && size != 0) {
        return D3DERR_INVALIDCALL;
    }
    Entry e;
    if (flags & D3DSPD_IUNKNOWN) {
        if (size != sizeof(IUnknown*)) {
            return D3DERR_INVALIDCALL;
        }
        e.unknown = *static_cast<IUnknown* const*>(data);
        if (e.unknown != nullptr) {
            e.unknown->AddRef();
        }
    } else {
        e.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    }
    free(guid);
    entries_.emplace(guid, std::move(e));
    return D3D_OK;
}

HRESULT PrivateData::get(REFGUID guid, void* data, DWORD* size) {
    if (size == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    auto it = entries_.find(guid);
    if (it == entries_.end()) {
        return D3DERR_NOTFOUND;
    }
    const bool isUnknown = it->second.bytes.empty() && it->second.unknown != nullptr;
    const DWORD needed = isUnknown ? DWORD(sizeof(IUnknown*)) : DWORD(it->second.bytes.size());
    if (data == nullptr) {
        *size = needed;
        return D3D_OK;
    }
    if (*size < needed) {
        *size = needed;
        return D3DERR_MOREDATA;
    }
    *size = needed;
    if (isUnknown) {
        it->second.unknown->AddRef();
        *static_cast<IUnknown**>(data) = it->second.unknown;
    } else if (needed != 0) {
        std::memcpy(data, it->second.bytes.data(), needed);
    }
    return D3D_OK;
}

HRESULT PrivateData::free(REFGUID guid) {
    auto it = entries_.find(guid);
    if (it == entries_.end()) {
        return D3DERR_NOTFOUND;
    }
    if (it->second.unknown != nullptr) {
        it->second.unknown->Release();
    }
    entries_.erase(it);
    return D3D_OK;
}

void PrivateData::clear() {
    for (auto& [g, e] : entries_) {
        if (e.unknown != nullptr) {
            e.unknown->Release();
        }
    }
    entries_.clear();
}

// ---- Subresource locking ----------------------------------------------------------------------------

void Subresource::init(uint32_t w, uint32_t h, uint32_t d, uint32_t fmt) {
    width = std::max(w, 1u);
    height = std::max(h, 1u);
    depth = std::max(d, 1u);
    format = fmt;
    layout = formatLayout(fmt);
}

uint8_t* Subresource::storage() {
    if (shadow.empty()) {
        shadow.assign(subresourceBytes(layout, width, height, depth), 0);
    }
    return shadow.data();
}

namespace {

bool regionFromWire(const Subresource& s, const std::vector<int32_t>& wr, Region& r) {
    if (wr.empty()) {
        r = Region {0, 0, s.width, s.height, 0, s.depth};
        return true;
    }
    for (int32_t v : wr) {
        if (v < 0) {
            return false;
        }
    }
    if (wr.size() == 4) {
        r = Region {uint32_t(wr[0]), uint32_t(wr[1]), uint32_t(wr[2]), uint32_t(wr[3]), 0, 1};
        return true;
    }
    if (wr.size() == 6) {
        r = Region {uint32_t(wr[0]), uint32_t(wr[1]), uint32_t(wr[2]), uint32_t(wr[3]), uint32_t(wr[4]), uint32_t(wr[5])};
        return true;
    }
    return false;
}

void copyRegion(uint8_t* base, const RegionLayout& rl, uint8_t* packed, bool toPacked) {
    size_t p = 0;
    for (uint32_t z = 0; z < rl.slices; ++z) {
        for (uint32_t y = 0; y < rl.rows; ++y) {
            uint8_t* row = base + rl.offset + z * rl.slicePitch + size_t(y) * rl.pitch;
            if (toPacked) {
                std::memcpy(packed + p, row, rl.rowBytes);
            } else {
                std::memcpy(row, packed + p, rl.rowBytes);
            }
            p += rl.rowBytes;
        }
    }
}

}  // namespace

HRESULT lockSubresource(Subresource& s, const std::vector<int32_t>& wr, DWORD flags, void** bits, INT* rowPitch,
                        INT* slicePitch, const ReadFn& read) {
    Region region;
    RegionLayout rl;
    if (bits == nullptr || !regionFromWire(s, wr, region) ||
        !regionLayout(s.layout, s.width, s.height, s.depth, region, rl)) {
        return D3DERR_INVALIDCALL;
    }
    uint8_t* base = s.storage();
    if (s.needsReadback(flags)) {
        std::vector<uint8_t> packed;
        const HRESULT hr = read(wr, packed);
        if (FAILED(hr)) {
            return hr;
        }
        if (packed.size() == size_t(rl.rowBytes) * rl.rows * rl.slices) {
            copyRegion(base, rl, packed.data(), false);
            const bool whole = region.left == 0 && region.top == 0 && region.right == s.width &&
                               region.bottom == s.height && region.front == 0 && region.back == s.depth;
            if (whole) {
                s.hostDirty = false;
            }
        } else {
            logf("fuse-relight bridge: read-back of %lu bytes, expected %lu\n", static_cast<unsigned long>(packed.size()),
                 static_cast<unsigned long>(size_t(rl.rowBytes) * rl.rows * rl.slices));
        }
    }
    *bits = base + rl.offset;
    *rowPitch = INT(rl.pitch);
    if (slicePitch != nullptr) {
        *slicePitch = INT(rl.slicePitch);
    }
    s.locks.push_back(Subresource::LockRecord {wr, rl, flags});
    return D3D_OK;
}

HRESULT unlockSubresource(Subresource& s, const UploadFn& upload) {
    if (s.locks.empty()) {
        return D3D_OK;  // upstream: engines unlock "just in case"
    }
    const Subresource::LockRecord rec = s.locks.front();
    s.locks.pop_front();
    if (rec.flags & D3DLOCK_READONLY) {
        return D3D_OK;
    }
    std::vector<uint8_t> packed(size_t(rec.rl.rowBytes) * rec.rl.rows * rec.rl.slices);
    copyRegion(s.storage(), rec.rl, packed.data(), true);
    upload(rec.wireRegion, rec.flags, rec.rl, packed);
    return D3D_OK;
}

void fillSubresource(Subresource& s, const std::vector<uint8_t>& packed) {
    RegionLayout rl;
    const Region whole {0, 0, s.width, s.height, 0, s.depth};
    if (!regionLayout(s.layout, s.width, s.height, s.depth, whole, rl) ||
        packed.size() != size_t(rl.rowBytes) * rl.rows * rl.slices) {
        logf("fuse-relight bridge: whole-surface read-back of %lu bytes does not match the surface\n",
             static_cast<unsigned long>(packed.size()));
        s.hostDirty = true;
        return;
    }
    copyRegion(s.storage(), rl, const_cast<uint8_t*>(packed.data()), false);
    s.hostDirty = false;
}

// ---- ResourceBase -----------------------------------------------------------------------------------

template <class Iface>
HRESULT STDMETHODCALLTYPE ResourceBase<Iface>::GetDevice(IDirect3DDevice9** ppDevice) {
    if (ppDevice == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    device()->AddRef();
    *ppDevice = device();
    return D3D_OK;
}

template <class Iface>
DWORD STDMETHODCALLTYPE ResourceBase<Iface>::SetPriority(DWORD priority) {
    FUSE_BRIDGE_LOCK();
    const DWORD old = priority_;
    priority_ = priority;
    cmd::IDirect3DResource9_SetPriority c;
    c.priority = priority;
    bridge().post(c, handle());
    return old;
}

template <class Iface>
void STDMETHODCALLTYPE ResourceBase<Iface>::PreLoad() {
    FUSE_BRIDGE_LOCK();
    bridge().post(cmd::IDirect3DResource9_PreLoad {}, handle());
}

template class ResourceBase<IDirect3DSurface9>;
template class ResourceBase<IDirect3DTexture9>;
template class ResourceBase<IDirect3DCubeTexture9>;
template class ResourceBase<IDirect3DVolumeTexture9>;
template class ResourceBase<IDirect3DVertexBuffer9>;
template class ResourceBase<IDirect3DIndexBuffer9>;

// ---- Surface ------------------------------------------------------------------------------------------

Surface::Surface(uint32_t handle, Device* dev, const D3DSURFACE_DESC& desc, BridgeObject* container, Subresource* shared)
    : ResourceBase(Kind::Surface, handle, dev, container, desc.Pool), desc_(desc), sub_(shared ? shared : &own_) {
    if (shared == nullptr) {
        own_.init(desc.Width, desc.Height, 1, desc.Format);
    }
    if (desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
        sub_->gpuWritable = true;
    }
}

HRESULT Surface::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DSurface9*>(this), riid, ppv,
                       {&__uuidof(IDirect3DResource9), &__uuidof(IDirect3DSurface9)});
}

HRESULT STDMETHODCALLTYPE Surface::GetContainer(REFIID riid, void** ppContainer) {
    if (ppContainer == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    IUnknown* owner = container() ? container()->unknown() : device()->unknown();
    return owner->QueryInterface(riid, ppContainer);
}

HRESULT STDMETHODCALLTYPE Surface::GetDesc(D3DSURFACE_DESC* pDesc) {
    if (pDesc == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *pDesc = desc_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Surface::LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) {
    FUSE_BRIDGE_LOCK();
    if (locked == nullptr || !sub_->locks.empty()) {
        return D3DERR_INVALIDCALL;
    }
    const uint32_t h = handle();
    return lockSubresource(*sub_, wire::rectToWire(rect), flags, &locked->pBits, &locked->Pitch, nullptr,
                           [&](const std::vector<int32_t>& wr, std::vector<uint8_t>& out) {
                               cmd::IDirect3DSurface9_LockRect c;
                               c.rect = wr;
                               c.flags = flags;
                               return bridge().readData(c, h, out, D3DERR_INVALIDCALL);
                           });
}

HRESULT STDMETHODCALLTYPE Surface::UnlockRect() {
    FUSE_BRIDGE_LOCK();
    const uint32_t h = handle();
    return unlockSubresource(*sub_, [&](const std::vector<int32_t>& wr, DWORD flags, const RegionLayout& rl,
                                        const std::vector<uint8_t>& packed) {
        cmd::IDirect3DSurface9_UnlockRect c;
        c.rect = wr;
        c.flags = flags;
        c.rowBytes = rl.rowBytes;
        c.rows = rl.rows;
        if (bridge().attachData(c, packed.data(), packed.size())) {
            bridge().post(c, h);
        }
    });
}

HRESULT STDMETHODCALLTYPE Surface::GetDC(HDC* phdc) {
    FUSE_BRIDGE_LOCK();
    if (phdc == nullptr || dc_.dc != nullptr) {
        return D3DERR_INVALIDCALL;
    }
    WORD bpp = 0;
    DWORD masks[3] = {0, 0, 0};
    switch (desc_.Format) {
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8: bpp = 32; break;
    case D3DFMT_R8G8B8: bpp = 24; break;
    case D3DFMT_R5G6B5: bpp = 16; masks[0] = 0xF800; masks[1] = 0x07E0; masks[2] = 0x001F; break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: bpp = 16; masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F; break;
    default: return D3DERR_INVALIDCALL;  // D3D9: GetDC supports these formats only
    }
    D3DLOCKED_RECT lr;
    HRESULT hr = LockRect(&lr, nullptr, 0);
    if (FAILED(hr)) {
        return hr;
    }
    struct {
        BITMAPINFOHEADER header;
        DWORD masks[3];
    } info {};
    info.header.biSize = sizeof(BITMAPINFOHEADER);
    info.header.biWidth = LONG(desc_.Width);
    info.header.biHeight = -LONG(desc_.Height);  // top-down, like the surface
    info.header.biPlanes = 1;
    info.header.biBitCount = bpp;
    info.header.biCompression = masks[0] ? BI_BITFIELDS : BI_RGB;
    std::memcpy(info.masks, masks, sizeof(masks));
    dc_.dc = ::CreateCompatibleDC(nullptr);
    dc_.bitmap = ::CreateDIBSection(dc_.dc, reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS, &dc_.bits, nullptr, 0);
    if (dc_.dc == nullptr || dc_.bitmap == nullptr) {
        if (dc_.dc) {
            ::DeleteDC(dc_.dc);
        }
        dc_ = DcState {};
        UnlockRect();
        return D3DERR_INVALIDCALL;
    }
    dc_.previous = ::SelectObject(dc_.dc, dc_.bitmap);
    const size_t dibPitch = ((size_t(desc_.Width) * bpp / 8) + 3) & ~size_t(3);
    const size_t rowBytes = size_t(desc_.Width) * bpp / 8;
    for (UINT y = 0; y < desc_.Height; ++y) {
        std::memcpy(static_cast<uint8_t*>(dc_.bits) + y * dibPitch, static_cast<uint8_t*>(lr.pBits) + y * size_t(lr.Pitch),
                    rowBytes);
    }
    *phdc = dc_.dc;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Surface::ReleaseDC(HDC hdc) {
    FUSE_BRIDGE_LOCK();
    if (hdc == nullptr || hdc != dc_.dc || sub_->locks.empty()) {
        return D3DERR_INVALIDCALL;
    }
    ::GdiFlush();
    const Subresource::LockRecord& rec = sub_->locks.front();
    const size_t bpp = rec.rl.rowBytes / std::max<UINT>(desc_.Width, 1);
    const size_t dibPitch = ((size_t(desc_.Width) * bpp) + 3) & ~size_t(3);
    uint8_t* base = sub_->storage() + rec.rl.offset;
    for (UINT y = 0; y < desc_.Height; ++y) {
        std::memcpy(base + y * size_t(rec.rl.pitch), static_cast<uint8_t*>(dc_.bits) + y * dibPitch, rec.rl.rowBytes);
    }
    ::SelectObject(dc_.dc, dc_.previous);
    ::DeleteObject(dc_.bitmap);
    ::DeleteDC(dc_.dc);
    dc_ = DcState {};
    return UnlockRect();
}

void Surface::destroyOnHost() {
    // Standalone surfaces are destroyed; children (levels, faces, back buffers) belong to their
    // container on the host and are only unlinked (upstream d3d9_surface.cpp onDestroy).
    if (container() != nullptr) {
        cmd::Bridge_UnlinkResource c;
        c.resource = handle();
        bridge().post(c, handle());
    } else {
        bridge().post(cmd::IDirect3DResource9_Destroy {}, handle());
    }
}

// ---- Volume -------------------------------------------------------------------------------------------

Volume::Volume(uint32_t handle, Device* dev, const D3DVOLUME_DESC& desc, BridgeObject* container, Subresource* shared)
    : BridgeObject(Kind::Volume, handle, dev, container), desc_(desc), sub_(shared) {}

HRESULT Volume::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DVolume9*>(this), riid, ppv, {&__uuidof(IDirect3DVolume9)});
}

HRESULT STDMETHODCALLTYPE Volume::GetDevice(IDirect3DDevice9** ppDevice) {
    if (ppDevice == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    device()->AddRef();
    *ppDevice = device();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Volume::SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) {
    FUSE_BRIDGE_LOCK();
    return privateData_.set(guid, data, size, flags);
}
HRESULT STDMETHODCALLTYPE Volume::GetPrivateData(REFGUID guid, void* data, DWORD* size) {
    FUSE_BRIDGE_LOCK();
    return privateData_.get(guid, data, size);
}
HRESULT STDMETHODCALLTYPE Volume::FreePrivateData(REFGUID guid) {
    FUSE_BRIDGE_LOCK();
    return privateData_.free(guid);
}

HRESULT STDMETHODCALLTYPE Volume::GetContainer(REFIID riid, void** ppContainer) {
    if (ppContainer == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    return container()->unknown()->QueryInterface(riid, ppContainer);
}

HRESULT STDMETHODCALLTYPE Volume::GetDesc(D3DVOLUME_DESC* pDesc) {
    if (pDesc == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *pDesc = desc_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Volume::LockBox(D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) {
    FUSE_BRIDGE_LOCK();
    if (locked == nullptr || !sub_->locks.empty()) {
        return D3DERR_INVALIDCALL;
    }
    const uint32_t h = handle();
    return lockSubresource(*sub_, wire::boxToWire(box), flags, &locked->pBits, &locked->RowPitch, &locked->SlicePitch,
                           [&](const std::vector<int32_t>& wr, std::vector<uint8_t>& out) {
                               cmd::IDirect3DVolume9_LockBox c;
                               c.box = wr;
                               c.flags = flags;
                               return bridge().readData(c, h, out, D3DERR_INVALIDCALL);
                           });
}

HRESULT STDMETHODCALLTYPE Volume::UnlockBox() {
    FUSE_BRIDGE_LOCK();
    const uint32_t h = handle();
    return unlockSubresource(*sub_, [&](const std::vector<int32_t>& wr, DWORD flags, const RegionLayout& rl,
                                        const std::vector<uint8_t>& packed) {
        cmd::IDirect3DVolume9_UnlockBox c;
        c.box = wr;
        c.flags = flags;
        c.rowBytes = rl.rowBytes;
        c.rows = rl.rows;
        c.slices = rl.slices;
        if (bridge().attachData(c, packed.data(), packed.size())) {
            bridge().post(c, h);
        }
    });
}

void Volume::destroyOnHost() {
    cmd::Bridge_UnlinkVolumeResource c;
    c.resource = handle();
    bridge().post(c, handle());
}

// ---- TextureBase ------------------------------------------------------------------------------------------

template <class Iface>
TextureBase<Iface>::TextureBase(Kind k, uint32_t h, Device* dev, const TextureDesc& desc, uint32_t faces)
    : ResourceBase<Iface>(k, h, dev, nullptr, desc.pool), desc_(desc), faces_(faces) {
    subs_.resize(size_t(faces) * desc_.levels);
    children_.assign(subs_.size(), nullptr);
    for (uint32_t f = 0; f < faces; ++f) {
        for (uint32_t l = 0; l < desc_.levels; ++l) {
            Subresource& s = sub(f, l);
            s.init(std::max(desc_.width >> l, 1u), std::max(desc_.height >> l, 1u), std::max(desc_.depth >> l, 1u),
                   desc_.format);
            s.gpuWritable = (desc_.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
        }
    }
}

template <class Iface>
DWORD STDMETHODCALLTYPE TextureBase<Iface>::SetLOD(DWORD lod) {
    FUSE_BRIDGE_LOCK();
    if (this->pool() != D3DPOOL_MANAGED) {
        return 0;  // D3D9: only managed textures have a LOD
    }
    const DWORD old = lod_;
    lod_ = std::min<DWORD>(lod, desc_.levels - 1);
    cmd::IDirect3DBaseTexture9_SetLOD c;
    c.lod = lod;
    bridge().post(c, this->handle());
    return old;
}

template <class Iface>
HRESULT STDMETHODCALLTYPE TextureBase<Iface>::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE filter) {
    FUSE_BRIDGE_LOCK();
    if (filter == D3DTEXF_NONE || filter > D3DTEXF_CONVOLUTIONMONO) {
        return D3DERR_INVALIDCALL;
    }
    filter_ = filter;
    cmd::IDirect3DBaseTexture9_SetAutoGenFilterType c;
    c.filterType = filter;
    return bridge().post(c, this->handle());
}

template <class Iface>
void STDMETHODCALLTYPE TextureBase<Iface>::GenerateMipSubLevels() {
    FUSE_BRIDGE_LOCK();
    if (desc_.usage & D3DUSAGE_AUTOGENMIPMAP) {
        bridge().post(cmd::IDirect3DBaseTexture9_GenerateMipSubLevels {}, this->handle());
        markHostDirty(true);
    }
}

template <class Iface>
void TextureBase<Iface>::markHostDirty(bool fromLevel1) noexcept {
    for (uint32_t f = 0; f < faces_; ++f) {
        for (uint32_t l = fromLevel1 ? 1 : 0; l < desc_.levels; ++l) {
            sub(f, l).hostDirty = true;
        }
    }
}

template <class Iface>
void TextureBase<Iface>::markGpuWritable() noexcept {
    for (Subresource& s : subs_) {
        s.gpuWritable = true;
    }
}

template <class Iface>
void TextureBase<Iface>::destroyOnHost() {
    bridge().post(cmd::IDirect3DResource9_Destroy {}, this->handle());
}

template <class Iface>
void TextureBase<Iface>::finalRelease() {
    for (BridgeObject*& c : children_) {
        if (c != nullptr) {
            c->teardownChild();
            c = nullptr;
        }
    }
    this->destroyOnHost();
    delete this;
}

template class TextureBase<IDirect3DTexture9>;
template class TextureBase<IDirect3DCubeTexture9>;
template class TextureBase<IDirect3DVolumeTexture9>;

// ---- Texture ------------------------------------------------------------------------------------------------

Texture::Texture(uint32_t h, Device* dev, const TextureDesc& desc) : TextureBase(Kind::Texture, h, dev, desc, 1) {}

HRESULT Texture::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DTexture9*>(this), riid, ppv,
                       {&__uuidof(IDirect3DResource9), &__uuidof(IDirect3DBaseTexture9), &__uuidof(IDirect3DTexture9)});
}

D3DSURFACE_DESC Texture::levelDesc(UINT level) const {
    D3DSURFACE_DESC d {};
    d.Format = desc_.format;
    d.Type = D3DRTYPE_SURFACE;
    d.Usage = desc_.usage;
    d.Pool = desc_.pool;
    d.MultiSampleType = D3DMULTISAMPLE_NONE;
    d.MultiSampleQuality = 0;
    d.Width = std::max(desc_.width >> level, 1u);
    d.Height = std::max(desc_.height >> level, 1u);
    return d;
}

HRESULT STDMETHODCALLTYPE Texture::GetLevelDesc(UINT level, D3DSURFACE_DESC* pDesc) {
    if (pDesc == nullptr || level >= desc_.levels) {
        return D3DERR_INVALIDCALL;
    }
    *pDesc = levelDesc(level);
    return D3D_OK;
}

Surface* Texture::level(UINT l) {
    if (children_[l] == nullptr) {
        auto* s = new Surface(newHandle(), device(), levelDesc(l), this, &sub(0, l));
        children_[l] = s;
        cmd::IDirect3DTexture9_GetSurfaceLevel c;
        c.level = l;
        c.result = s->handle();
        bridge().post(c, handle());
    }
    return static_cast<Surface*>(children_[l]);
}

HRESULT STDMETHODCALLTYPE Texture::GetSurfaceLevel(UINT l, IDirect3DSurface9** ppSurface) {
    FUSE_BRIDGE_LOCK();
    if (ppSurface == nullptr || l >= desc_.levels) {
        return D3DERR_INVALIDCALL;
    }
    Surface* s = level(l);
    s->addRef();
    *ppSurface = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Texture::LockRect(UINT l, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) {
    FUSE_BRIDGE_LOCK();
    if (locked == nullptr || l >= desc_.levels || !sub(0, l).locks.empty()) {
        return D3DERR_INVALIDCALL;
    }
    const uint32_t h = handle();
    return lockSubresource(sub(0, l), wire::rectToWire(rect), flags, &locked->pBits, &locked->Pitch, nullptr,
                           [&](const std::vector<int32_t>& wr, std::vector<uint8_t>& out) {
                               cmd::IDirect3DTexture9_LockRect c;
                               c.level = l;
                               c.rect = wr;
                               c.flags = flags;
                               return bridge().readData(c, h, out, D3DERR_INVALIDCALL);
                           });
}

HRESULT STDMETHODCALLTYPE Texture::UnlockRect(UINT l) {
    FUSE_BRIDGE_LOCK();
    if (l >= desc_.levels) {
        return D3DERR_INVALIDCALL;
    }
    const uint32_t h = handle();
    return unlockSubresource(sub(0, l), [&](const std::vector<int32_t>& wr, DWORD flags, const RegionLayout& rl,
                                            const std::vector<uint8_t>& packed) {
        cmd::IDirect3DTexture9_UnlockRect c;
        c.level = l;
        c.rect = wr;
        c.flags = flags;
        c.rowBytes = rl.rowBytes;
        c.rows = rl.rows;
        if (bridge().attachData(c, packed.data(), packed.size())) {
            bridge().post(c, h);
        }
    });
}

HRESULT STDMETHODCALLTYPE Texture::AddDirtyRect(const RECT* rect) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DTexture9_AddDirtyRect c;
    c.rect = wire::rectToWire(rect);
    return bridge().post(c, handle());
}

// ---- CubeTexture ------------------------------------------------------------------------------------------------

CubeTexture::CubeTexture(uint32_t h, Device* dev, const TextureDesc& desc)
    : TextureBase(Kind::CubeTexture, h, dev, desc, 6) {}

HRESULT CubeTexture::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DCubeTexture9*>(this), riid, ppv,
                       {&__uuidof(IDirect3DResource9), &__uuidof(IDirect3DBaseTexture9), &__uuidof(IDirect3DCubeTexture9)});
}

D3DSURFACE_DESC CubeTexture::levelDesc(UINT level) const {
    D3DSURFACE_DESC d {};
    d.Format = desc_.format;
    d.Type = D3DRTYPE_SURFACE;
    d.Usage = desc_.usage;
    d.Pool = desc_.pool;
    d.Width = std::max(desc_.width >> level, 1u);
    d.Height = d.Width;
    return d;
}

HRESULT STDMETHODCALLTYPE CubeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC* pDesc) {
    if (pDesc == nullptr || level >= desc_.levels) {
        return D3DERR_INVALIDCALL;
    }
    *pDesc = levelDesc(level);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT l, IDirect3DSurface9** ppSurface) {
    FUSE_BRIDGE_LOCK();
    if (ppSurface == nullptr || uint32_t(face) >= 6 || l >= desc_.levels) {
        return D3DERR_INVALIDCALL;
    }
    const size_t i = size_t(face) * desc_.levels + l;
    if (children_[i] == nullptr) {
        auto* s = new Surface(newHandle(), device(), levelDesc(l), this, &sub(face, l));
        children_[i] = s;
        cmd::IDirect3DCubeTexture9_GetCubeMapSurface c;
        c.face = face;
        c.level = l;
        c.result = s->handle();
        bridge().post(c, handle());
    }
    children_[i]->addRef();
    *ppSurface = static_cast<Surface*>(children_[i]);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE CubeTexture::LockRect(D3DCUBEMAP_FACES face, UINT l, D3DLOCKED_RECT* locked, const RECT* rect,
                                                DWORD flags) {
    FUSE_BRIDGE_LOCK();
    if (locked == nullptr || uint32_t(face) >= 6 || l >= desc_.levels || !sub(face, l).locks.empty()) {
        return D3DERR_INVALIDCALL;
    }
    const uint32_t h = handle();
    return lockSubresource(sub(face, l), wire::rectToWire(rect), flags, &locked->pBits, &locked->Pitch, nullptr,
                           [&](const std::vector<int32_t>& wr, std::vector<uint8_t>& out) {
                               cmd::IDirect3DCubeTexture9_LockRect c;
                               c.face = face;
                               c.level = l;
                               c.rect = wr;
                               c.flags = flags;
                               return bridge().readData(c, h, out, D3DERR_INVALIDCALL);
                           });
}

HRESULT STDMETHODCALLTYPE CubeTexture::UnlockRect(D3DCUBEMAP_FACES face, UINT l) {
    FUSE_BRIDGE_LOCK();
    if (uint32_t(face) >= 6 || l >= desc_.levels) {
        return D3DERR_INVALIDCALL;
    }
    const uint32_t h = handle();
    return unlockSubresource(sub(face, l), [&](const std::vector<int32_t>& wr, DWORD flags, const RegionLayout& rl,
                                               const std::vector<uint8_t>& packed) {
        cmd::IDirect3DCubeTexture9_UnlockRect c;
        c.face = face;
        c.level = l;
        c.rect = wr;
        c.flags = flags;
        c.rowBytes = rl.rowBytes;
        c.rows = rl.rows;
        if (bridge().attachData(c, packed.data(), packed.size())) {
            bridge().post(c, h);
        }
    });
}

HRESULT STDMETHODCALLTYPE CubeTexture::AddDirtyRect(D3DCUBEMAP_FACES face, const RECT* rect) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DCubeTexture9_AddDirtyRect c;
    c.face = face;
    c.rect = wire::rectToWire(rect);
    return bridge().post(c, handle());
}

// ---- VolumeTexture ------------------------------------------------------------------------------------------------

VolumeTexture::VolumeTexture(uint32_t h, Device* dev, const TextureDesc& desc)
    : TextureBase(Kind::VolumeTexture, h, dev, desc, 1) {}

HRESULT VolumeTexture::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DVolumeTexture9*>(this), riid, ppv,
                       {&__uuidof(IDirect3DResource9), &__uuidof(IDirect3DBaseTexture9),
                        &__uuidof(IDirect3DVolumeTexture9)});
}

D3DVOLUME_DESC VolumeTexture::levelDesc(UINT level) const {
    D3DVOLUME_DESC d {};
    d.Format = desc_.format;
    d.Type = D3DRTYPE_VOLUME;
    d.Usage = desc_.usage;
    d.Pool = desc_.pool;
    d.Width = std::max(desc_.width >> level, 1u);
    d.Height = std::max(desc_.height >> level, 1u);
    d.Depth = std::max(desc_.depth >> level, 1u);
    return d;
}

HRESULT STDMETHODCALLTYPE VolumeTexture::GetLevelDesc(UINT level, D3DVOLUME_DESC* pDesc) {
    if (pDesc == nullptr || level >= desc_.levels) {
        return D3DERR_INVALIDCALL;
    }
    *pDesc = levelDesc(level);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE VolumeTexture::GetVolumeLevel(UINT l, IDirect3DVolume9** ppVolume) {
    FUSE_BRIDGE_LOCK();
    if (ppVolume == nullptr || l >= desc_.levels) {
        return D3DERR_INVALIDCALL;
    }
    if (children_[l] == nullptr) {
        auto* v = new Volume(newHandle(), device(), levelDesc(l), this, &sub(0, l));
        children_[l] = v;
        cmd::IDirect3DVolumeTexture9_GetVolumeLevel c;
        c.level = l;
        c.result = v->handle();
        bridge().post(c, handle());
    }
    children_[l]->addRef();
    *ppVolume = static_cast<Volume*>(children_[l]);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE VolumeTexture::LockBox(UINT l, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) {
    FUSE_BRIDGE_LOCK();
    if (locked == nullptr || l >= desc_.levels || !sub(0, l).locks.empty()) {
        return D3DERR_INVALIDCALL;
    }
    const uint32_t h = handle();
    return lockSubresource(sub(0, l), wire::boxToWire(box), flags, &locked->pBits, &locked->RowPitch,
                           &locked->SlicePitch, [&](const std::vector<int32_t>& wr, std::vector<uint8_t>& out) {
                               cmd::IDirect3DVolumeTexture9_LockBox c;
                               c.level = l;
                               c.box = wr;
                               c.flags = flags;
                               return bridge().readData(c, h, out, D3DERR_INVALIDCALL);
                           });
}

HRESULT STDMETHODCALLTYPE VolumeTexture::UnlockBox(UINT l) {
    FUSE_BRIDGE_LOCK();
    if (l >= desc_.levels) {
        return D3DERR_INVALIDCALL;
    }
    const uint32_t h = handle();
    return unlockSubresource(sub(0, l), [&](const std::vector<int32_t>& wr, DWORD flags, const RegionLayout& rl,
                                            const std::vector<uint8_t>& packed) {
        cmd::IDirect3DVolumeTexture9_UnlockBox c;
        c.level = l;
        c.box = wr;
        c.flags = flags;
        c.rowBytes = rl.rowBytes;
        c.rows = rl.rows;
        c.slices = rl.slices;
        if (bridge().attachData(c, packed.data(), packed.size())) {
            bridge().post(c, h);
        }
    });
}

HRESULT STDMETHODCALLTYPE VolumeTexture::AddDirtyBox(const D3DBOX* box) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DVolumeTexture9_AddDirtyBox c;
    c.box = wire::boxToWire(box);
    return bridge().post(c, handle());
}

// ---- Buffers ----------------------------------------------------------------------------------------------------

template <class Iface, class Desc>
Buffer<Iface, Desc>::Buffer(Kind k, uint32_t h, Device* dev, const Desc& desc)
    : ResourceBase<Iface>(k, h, dev, nullptr, desc.Pool), desc_(desc) {}

template <class Iface, class Desc>
HRESULT STDMETHODCALLTYPE Buffer<Iface, Desc>::Lock(UINT offset, UINT size, void** ppData, DWORD flags) {
    FUSE_BRIDGE_LOCK();
    if (ppData == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    if (offset > desc_.Size) {
        return D3DERR_INVALIDCALL;
    }
    // Upstream lockable_buffer.h: size 0 locks to the end; oversized requests are clamped.
    if (size == 0 || size > desc_.Size - offset) {
        size = desc_.Size - offset;
    }
    if (shadow_.empty()) {
        shadow_.assign(desc_.Size, 0);
    }
    const bool mayRead = !(desc_.Usage & D3DUSAGE_WRITEONLY) && !(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE));
    if (hostDirty_ && mayRead && size != 0) {
        std::vector<uint8_t> data;
        const bool isVb = std::is_same_v<Iface, IDirect3DVertexBuffer9>;
        HRESULT hr;
        if (isVb) {
            cmd::IDirect3DVertexBuffer9_Lock c;
            c.offset = offset;
            c.size = size;
            c.flags = flags;
            hr = bridge().readData(c, this->handle(), data, D3DERR_INVALIDCALL);
        } else {
            cmd::IDirect3DIndexBuffer9_Lock c;
            c.offset = offset;
            c.size = size;
            c.flags = flags;
            hr = bridge().readData(c, this->handle(), data, D3DERR_INVALIDCALL);
        }
        if (FAILED(hr)) {
            return hr;
        }
        if (data.size() == size) {
            std::memcpy(shadow_.data() + offset, data.data(), size);
            if (offset == 0 && size == desc_.Size) {
                hostDirty_ = false;
            }
        }
    }
    *ppData = shadow_.data() + offset;
    locks_.push_back(LockRecord {offset, size, flags});
    return D3D_OK;
}

template <class Iface, class Desc>
HRESULT STDMETHODCALLTYPE Buffer<Iface, Desc>::Unlock() {
    FUSE_BRIDGE_LOCK();
    if (locks_.empty()) {
        return D3D_OK;
    }
    const LockRecord rec = locks_.front();
    locks_.pop_front();
    if ((rec.flags & D3DLOCK_READONLY) || rec.size == 0) {
        return D3D_OK;
    }
    const uint8_t* p = shadow_.data() + rec.offset;
    if constexpr (std::is_same_v<Iface, IDirect3DVertexBuffer9>) {
        cmd::IDirect3DVertexBuffer9_Unlock c;
        c.offset = rec.offset;
        c.flags = rec.flags;
        if (bridge().attachData(c, p, rec.size)) {
            bridge().post(c, this->handle());
        }
    } else {
        cmd::IDirect3DIndexBuffer9_Unlock c;
        c.offset = rec.offset;
        c.flags = rec.flags;
        if (bridge().attachData(c, p, rec.size)) {
            bridge().post(c, this->handle());
        }
    }
    return D3D_OK;
}

template <class Iface, class Desc>
HRESULT STDMETHODCALLTYPE Buffer<Iface, Desc>::GetDesc(Desc* pDesc) {
    if (pDesc == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *pDesc = desc_;
    return D3D_OK;
}

template <class Iface, class Desc>
void Buffer<Iface, Desc>::destroyOnHost() {
    bridge().post(cmd::IDirect3DResource9_Destroy {}, this->handle());
}

template class Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>;
template class Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>;

HRESULT VertexBuffer::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DVertexBuffer9*>(this), riid, ppv,
                       {&__uuidof(IDirect3DResource9), &__uuidof(IDirect3DVertexBuffer9)});
}

HRESULT IndexBuffer::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DIndexBuffer9*>(this), riid, ppv,
                       {&__uuidof(IDirect3DResource9), &__uuidof(IDirect3DIndexBuffer9)});
}

// ---- SwapChain ----------------------------------------------------------------------------------------------------

SwapChain::SwapChain(uint32_t h, Device* dev, const D3DPRESENT_PARAMETERS& pp, uint32_t implicitIndex)
    : BridgeObject(Kind::SwapChain, h, dev, nullptr), pp_(pp), implicitIndex_(implicitIndex) {
    backBuffers_.assign(std::max<UINT>(pp_.BackBufferCount, 1), nullptr);
}

HRESULT SwapChain::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DSwapChain9Ex*>(this), riid, ppv,
                       {&__uuidof(IDirect3DSwapChain9), &__uuidof(IDirect3DSwapChain9Ex)});
}

Surface* SwapChain::backBuffer(UINT index) {
    if (index >= backBuffers_.size()) {
        return nullptr;
    }
    if (backBuffers_[index] == nullptr) {
        D3DSURFACE_DESC d {};
        d.Format = pp_.BackBufferFormat;
        d.Type = D3DRTYPE_SURFACE;
        d.Usage = D3DUSAGE_RENDERTARGET;
        d.Pool = D3DPOOL_DEFAULT;
        d.MultiSampleType = pp_.MultiSampleType;
        d.MultiSampleQuality = pp_.MultiSampleQuality;
        d.Width = pp_.BackBufferWidth;
        d.Height = pp_.BackBufferHeight;
        auto* s = new Surface(newHandle(), device(), d, this, nullptr);
        backBuffers_[index] = s;
        cmd::IDirect3DSwapChain9_GetBackBuffer c;
        c.backBuffer = index;
        c.type = D3DBACKBUFFER_TYPE_MONO;
        c.result = s->handle();
        bridge().post(c, handle());
    }
    return backBuffers_[index];
}

HRESULT STDMETHODCALLTYPE SwapChain::Present(const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty,
                                             DWORD flags) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DSwapChain9_Present c;
    c.sourceRect = wire::rectToWire(src);
    c.destRect = wire::rectToWire(dst);
    c.destWindowOverride = wire::handleToWire(window);
    c.dirtyRegion = wire::regionToWire(dirty);
    c.flags = flags;
    bridge().send(c, handle(), 0);  // asynchronous, as Device::Present
    return bridge().alive() ? D3D_OK : D3DERR_DEVICELOST;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFrontBufferData(IDirect3DSurface9* dst) {
    FUSE_BRIDGE_LOCK();
    Surface* d = bridgeCast<Surface>(dst);
    if (d == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::IDirect3DSwapChain9_GetFrontBufferData c;
    c.destination = d->handle();
    return readWholeSurface(c, handle(), d);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE, IDirect3DSurface9** ppBackBuffer) {
    FUSE_BRIDGE_LOCK();
    if (ppBackBuffer == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    Surface* s = backBuffer(index);
    if (s == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    s->addRef();
    *ppBackBuffer = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetRasterStatus(D3DRASTER_STATUS* status) {
    FUSE_BRIDGE_LOCK();
    if (status == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::Reply_RasterStatus r;
    if (!bridge().query(cmd::IDirect3DSwapChain9_GetRasterStatus {}, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    status->InVBlank = BOOL(r.inVBlank);
    status->ScanLine = r.scanLine;
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDisplayMode(D3DDISPLAYMODE* mode) {
    FUSE_BRIDGE_LOCK();
    if (mode == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::Reply_DisplayMode r;
    if (!bridge().query(cmd::IDirect3DSwapChain9_GetDisplayMode {}, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    *mode = D3DDISPLAYMODE {r.width, r.height, r.refreshRate, D3DFORMAT(r.format)};
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDevice(IDirect3DDevice9** ppDevice) {
    if (ppDevice == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    device()->AddRef();
    *ppDevice = device();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS* pp) {
    if (pp == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *pp = pp_;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetLastPresentCount(UINT* count) {
    FUSE_BRIDGE_LOCK();
    if (count == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    cmd::Reply_Value r;
    if (!bridge().query(cmd::IDirect3DSwapChain9_GetLastPresentCount {}, handle(), r)) {
        return D3DERR_INVALIDCALL;
    }
    *count = r.value;
    return HRESULT(r.hresult);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetPresentStats(D3DPRESENTSTATS*) {
    return D3DERR_INVALIDCALL;  // DXVK does not implement it either
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDisplayModeEx(D3DDISPLAYMODEEX* mode, D3DDISPLAYROTATION* rotation) {
    FUSE_BRIDGE_LOCK();
    cmd::Reply_DisplayMode r;
    if (!bridge().query(cmd::IDirect3DSwapChain9_GetDisplayModeEx {}, handle(), r)) {
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

void SwapChain::destroyOnHost() { bridge().post(cmd::IDirect3DSwapChain9_Destroy {}, handle()); }

void SwapChain::finalRelease() {
    for (Surface*& s : backBuffers_) {
        if (s != nullptr) {
            s->teardownChild();
            s = nullptr;
        }
    }
    destroyOnHost();
    delete this;
}

// ---- StateBlock ---------------------------------------------------------------------------------------------------

StateBlock::StateBlock(uint32_t h, Device* dev, bool softwareVp) : BridgeObject(Kind::StateBlock, h, dev, nullptr) {
    state.sizeConstants(softwareVp);
    mask.sizeConstants(softwareVp);
}

HRESULT StateBlock::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DStateBlock9*>(this), riid, ppv, {&__uuidof(IDirect3DStateBlock9)});
}

HRESULT STDMETHODCALLTYPE StateBlock::GetDevice(IDirect3DDevice9** ppDevice) {
    if (ppDevice == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    device()->AddRef();
    *ppDevice = device();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE StateBlock::Capture() {
    FUSE_BRIDGE_LOCK();
    if (device()->recording()) {
        return D3DERR_INVALIDCALL;
    }
    transferState(mask, device()->shadowState(), state);
    return bridge().post(cmd::IDirect3DStateBlock9_Capture {}, handle());
}

HRESULT STDMETHODCALLTYPE StateBlock::Apply() {
    FUSE_BRIDGE_LOCK();
    if (device()->recording()) {
        return D3DERR_INVALIDCALL;
    }
    transferState(mask, state, device()->shadowState());
    return bridge().post(cmd::IDirect3DStateBlock9_Apply {}, handle());
}

void StateBlock::destroyOnHost() { bridge().post(cmd::IDirect3DStateBlock9_Destroy {}, handle()); }

// ---- VertexDeclaration --------------------------------------------------------------------------------------------

VertexDeclaration::VertexDeclaration(uint32_t h, Device* dev, std::vector<D3DVERTEXELEMENT9> elements)
    : BridgeObject(Kind::VertexDeclaration, h, dev, nullptr), elements_(std::move(elements)) {}

HRESULT VertexDeclaration::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DVertexDeclaration9*>(this), riid, ppv,
                       {&__uuidof(IDirect3DVertexDeclaration9)});
}

HRESULT STDMETHODCALLTYPE VertexDeclaration::GetDevice(IDirect3DDevice9** ppDevice) {
    if (ppDevice == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    device()->AddRef();
    *ppDevice = device();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE VertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9* elements, UINT* count) {
    if (count == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    *count = UINT(elements_.size());
    if (elements != nullptr) {
        std::memcpy(elements, elements_.data(), elements_.size() * sizeof(D3DVERTEXELEMENT9));
    }
    return D3D_OK;
}

void VertexDeclaration::destroyOnHost() { bridge().post(cmd::IDirect3DVertexDeclaration9_Destroy {}, handle()); }

// ---- Shaders -------------------------------------------------------------------------------------------------------

template <class Iface, Kind K>
HRESULT Shader<Iface, K>::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<Iface*>(this), riid, ppv, {&__uuidof(Iface)});
}

template <class Iface, Kind K>
HRESULT STDMETHODCALLTYPE Shader<Iface, K>::GetDevice(IDirect3DDevice9** ppDevice) {
    if (ppDevice == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    device()->AddRef();
    *ppDevice = device();
    return D3D_OK;
}

template <class Iface, Kind K>
HRESULT STDMETHODCALLTYPE Shader<Iface, K>::GetFunction(void* data, UINT* size) {
    if (size == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    const UINT bytes = UINT(code_.size() * sizeof(DWORD));
    if (data == nullptr) {
        *size = bytes;
        return D3D_OK;
    }
    if (*size < bytes) {
        return D3DERR_MOREDATA;
    }
    std::memcpy(data, code_.data(), bytes);
    *size = bytes;
    return D3D_OK;
}

template <class Iface, Kind K>
void Shader<Iface, K>::destroyOnHost() {
    if constexpr (K == Kind::VertexShader) {
        bridge().post(cmd::IDirect3DVertexShader9_Destroy {}, handle());
    } else {
        bridge().post(cmd::IDirect3DPixelShader9_Destroy {}, handle());
    }
}

template class Shader<IDirect3DVertexShader9, Kind::VertexShader>;
template class Shader<IDirect3DPixelShader9, Kind::PixelShader>;

// ---- Query ---------------------------------------------------------------------------------------------------------

Query::Query(uint32_t h, Device* dev, D3DQUERYTYPE type) : BridgeObject(Kind::Query, h, dev, nullptr), type_(type) {}

HRESULT Query::query(REFIID riid, void** ppv) {
    return queryCommon(this, static_cast<IDirect3DQuery9*>(this), riid, ppv, {&__uuidof(IDirect3DQuery9)});
}

HRESULT STDMETHODCALLTYPE Query::GetDevice(IDirect3DDevice9** ppDevice) {
    if (ppDevice == nullptr) {
        return D3DERR_INVALIDCALL;
    }
    device()->AddRef();
    *ppDevice = device();
    return D3D_OK;
}

DWORD Query::dataSize(D3DQUERYTYPE type) {
    switch (type) {
    case D3DQUERYTYPE_VCACHE: return sizeof(D3DDEVINFO_VCACHE);
    case D3DQUERYTYPE_RESOURCEMANAGER: return sizeof(D3DDEVINFO_D3DRESOURCEMANAGER);
    case D3DQUERYTYPE_VERTEXSTATS: return sizeof(D3DDEVINFO_D3DVERTEXSTATS);
    case D3DQUERYTYPE_EVENT: return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION: return sizeof(DWORD);
    case D3DQUERYTYPE_TIMESTAMP: return sizeof(UINT64);
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: return sizeof(BOOL);
    case D3DQUERYTYPE_TIMESTAMPFREQ: return sizeof(UINT64);
    case D3DQUERYTYPE_PIPELINETIMINGS: return sizeof(D3DDEVINFO_D3D9PIPELINETIMINGS);
    case D3DQUERYTYPE_INTERFACETIMINGS: return sizeof(D3DDEVINFO_D3D9INTERFACETIMINGS);
    case D3DQUERYTYPE_VERTEXTIMINGS:
    case D3DQUERYTYPE_PIXELTIMINGS: return sizeof(D3DDEVINFO_D3D9STAGETIMINGS);
    case D3DQUERYTYPE_BANDWIDTHTIMINGS: return sizeof(D3DDEVINFO_D3D9BANDWIDTHTIMINGS);
    case D3DQUERYTYPE_CACHEUTILIZATION: return sizeof(D3DDEVINFO_D3D9CACHEUTILIZATION);
    default: return 0;
    }
}

DWORD STDMETHODCALLTYPE Query::GetDataSize() { return dataSize(type_); }

HRESULT STDMETHODCALLTYPE Query::Issue(DWORD flags) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DQuery9_Issue c;
    c.issueFlags = flags;
    return bridge().post(c, handle());
}

HRESULT STDMETHODCALLTYPE Query::GetData(void* data, DWORD size, DWORD flags) {
    FUSE_BRIDGE_LOCK();
    cmd::IDirect3DQuery9_GetData c;
    c.size = size;
    c.getDataFlags = flags;
    std::vector<uint8_t> out;
    const HRESULT hr = bridge().readData(c, handle(), out, D3DERR_DEVICELOST);
    if (hr == S_OK && data != nullptr && size != 0) {
        std::memcpy(data, out.data(), std::min<size_t>(size, out.size()));
    }
    return hr;
}

void Query::destroyOnHost() { bridge().post(cmd::IDirect3DQuery9_Destroy {}, handle()); }

}  // namespace fuse::relight::bridge::client
