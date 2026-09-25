// FUSE Relight RL-1.1: the tap dispatcher inside the vendored DXVK d3d9.dll. Converts DXVK's D3D9
// state into IRelightTap events (relight_tap.hpp) and assigns resource ids. FUSE code, compiled
// into relight_d3d9 with DXVK's headers as system headers (see tap/CMakeLists.txt).
//
// Threading: every entry point runs under the D3D9 device lock (D3D9DeviceLock), except
// QueryIssue (D3D9 issues queries without it) - the context's own mutex covers that path.
#include <d3d9_device.h>
#include <d3d9_buffer.h>
#include <d3d9_query.h>
#include <d3d9_shader.h>
#include <d3d9_surface.h>
#include <d3d9_swapchain.h>
#include <d3d9_texture.h>
#include <d3d9_vertex_declaration.h>

#include <fuse/relight/tap/d3d9_names.hpp>
#include <fuse/relight/tap/device_tap.hpp>
#include <fuse/relight/tap/frame_host.hpp>
#include <fuse/relight/tap/relight_tap.hpp>
#include <fuse/relight/tap/tap_config.hpp>
#include <fuse/relight/tap/vk_bootstrap.hpp>
#if defined(FUSE_RELIGHT_HAVE_VERTEX_CAPTURE)
#include "fuse_vertex_capture_dxvk.h" // RL-1.6 vertex capture (Source/FUSE/Relight/capture/vertex_capture/dxvk)
#endif

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxvk {

namespace rt = fuse::relight::tap;

namespace {

template <typename T>
struct ShaderEntry {
    Com<T, false> ref; ///< private reference: the pointer cannot be recycled while cached
    rt::ResourceId id = rt::kNoResource;
    std::vector<uint32_t> tokens;
    uint32_t byteSize = 0;
};

struct PendingBufferWrite {
    uint32_t offset = 0, size = 0, flags = 0;
};

struct BufferEntry {
    rt::ResourceId id = rt::kNoResource;
    std::vector<PendingBufferWrite> pending;
};

struct TextureLockEntry {
    const void* data = nullptr;
    uint32_t rowPitch = 0, slicePitch = 0, rows = 0, flags = 0;
    rt::Box box;
    bool full = false;
};

std::atomic<unsigned> g_deviceOrdinal{0};

bool isBlockCompressed(D3D9Format f) {
    switch (f) {
    case D3D9Format::DXT1:
    case D3D9Format::DXT2:
    case D3D9Format::DXT3:
    case D3D9Format::DXT4:
    case D3D9Format::DXT5:
    case D3D9Format::ATI1:
    case D3D9Format::ATI2:
        return true;
    default:
        return false;
    }
}

} // namespace

/// A passthrough texture swap (RL-4.1): the FUSE-owned twin DXVK samples instead of the texture.
struct TextureSwap {
    Rc<DxvkImage> image;               ///< the twin, imported from FUSE
    uint64_t version = 1;              ///< the texture's content version (bumped on every write we see)
    uint64_t copiedVersion = 0;        ///< the version the twin holds
    const DxvkImage* copiedFrom = nullptr; ///< the texture image it was copied from (DXVK may replace it)
};

/// Per-device dispatcher state (D3D9DeviceEx::m_fuseTap). It is also the device's RL-4.1 frame host
/// (frame_host.hpp): FUSE's injection, composite, timeline sync and texture swap go through DXVK's own
/// command stream (EmitCs), so they are ordered with the application's calls.
class FuseTapContext final : public rt::IFrameHost {
public:
    ~FuseTapContext() override;

    D3D9DeviceEx* device = nullptr;
    std::unique_ptr<rt::IRelightTap> tap;
    std::mutex queryMutex;
    std::unordered_map<const D3D9CommonTexture*, rt::ResourceId> textures;
    std::unordered_map<rt::ResourceId, D3D9CommonTexture*> texturesById;
    rt::ResourceId nextTexture = 1;
    std::unordered_map<const D3D9CommonBuffer*, BufferEntry> buffers;
    rt::ResourceId nextBuffer = 1;
    std::unordered_map<const D3D9VertexShader*, ShaderEntry<D3D9VertexShader>> vertexShaders;
    std::unordered_map<const D3D9PixelShader*, ShaderEntry<D3D9PixelShader>> pixelShaders;
    rt::ResourceId nextShader = 1;
    std::map<std::pair<const D3D9CommonTexture*, UINT>, TextureLockEntry> locks;
    std::vector<rt::Light> lights;
    uint64_t frame = 0;
    // DrawState change counters (never 0: 0 means "not tracked"). lightsVersion advances in LightsChanged
    // (patches RL-1.1-24/25); clipPlanesVersion when DXVK's own ClipPlanes dirty flag is set at a draw,
    // i.e. SetClipPlane / SetRenderState(CLIPPLANEENABLE) / ResetState since DXVK's last PrepareDraw.
    // Both advance at device creation and reset (Remix's ResetState dirties both).
    uint32_t lightsVersion = 1;
    uint32_t clipPlanesVersion = 1;
    uint64_t drawOrdinal = 0; ///< onDraw calls so far (RL-1.6 VertexCaptureDraw::draw)
#if defined(FUSE_RELIGHT_HAVE_VERTEX_CAPTURE)
    /// RL-1.6: capture regions and the SPIR-V substitutor, when the tap wants vertex capture.
    std::unique_ptr<FuseVertexCapture> vertexCapture;
#endif

    rt::ResourceId textureId(D3D9CommonTexture* t);
    rt::ResourceId bufferId(D3D9CommonBuffer* b);
    void describeTexture(D3D9CommonTexture* t, rt::ResourceId id);
    rt::DeviceEvent deviceEvent(D3D9DeviceEx* dev, const D3DPRESENT_PARAMETERS* pp);
    template <typename T>
    rt::ShaderRef shaderRef(std::unordered_map<const T*, ShaderEntry<T>>& cache, T* shader);

    // ---- RL-4.1 frame host -------------------------------------------------------------------------------
    uint64_t getInstanceProcAddr() const override;
    rt::VulkanDevice vulkan() const override;
    bool deviceCreateInfo(rt::HostDeviceInfo& out) const override;
    uint64_t acquireSemaphore() override;
    uint64_t releaseSemaphore() override;
    bool backBufferInfo(rt::HostImageInfo& out) const override;
    bool textureInfo(rt::ResourceId texture, rt::HostImageInfo& out) const override;
    rt::HostImageHandle importImage(const rt::FuseImage& image) override;
    void releaseImage(rt::HostImageHandle image) override;
    bool copyBackBuffer(rt::HostImageHandle dst) override;
    bool flushAndSignal(uint64_t acquireValue) override;
    bool composite(rt::HostImageHandle src, uint64_t releaseValue) override;
    bool setTextureSwap(rt::ResourceId texture, const rt::FuseImage* image) override;
    void lockQueue() override;
    void unlockQueue() override;
    bool waitIdle() override;

    /// The texture's content changed (upload, UpdateTexture / UpdateSurface destination): a swapped texture's
    /// twin is refreshed at its next bind.
    void contentChanged(D3D9CommonTexture* t);
    /// The texture is being destroyed: its swap ends.
    void textureDestroyed(D3D9CommonTexture* t);
    bool bindSwapped(DWORD sampler, D3D9CommonTexture* t, bool srgb);

private:
    void ensureFences();
    Rc<DxvkImage> backBuffer() const;
    void markBindingsDirty(const D3D9CommonTexture* t);

    Rc<DxvkFence> m_acquire, m_release;
    std::unordered_map<rt::HostImageHandle, Rc<DxvkImage>> m_images;
    rt::HostImageHandle m_nextImage = 1;
    std::unordered_map<const D3D9CommonTexture*, TextureSwap> m_swaps;
};

void FuseTapContext::describeTexture(D3D9CommonTexture* t, rt::ResourceId id) {
    const D3D9_COMMON_TEXTURE_DESC* d = t->Desc();
    rt::TextureDesc desc;
    desc.id = id;
    desc.type = uint32_t(t->GetType());
    desc.width = d->Width;
    desc.height = d->Height;
    desc.depth = d->Depth;
    desc.mipLevels = d->MipLevels;
    desc.arraySize = d->ArraySize;
    desc.format = uint32_t(d->Format);
    desc.usage = d->Usage;
    desc.pool = uint32_t(d->Pool);
    desc.multiSample = uint32_t(d->MultiSample);
    desc.isBackBuffer = d->IsBackBuffer;
    desc.isAttachmentOnly = d->IsAttachmentOnly;
    const Rc<DxvkImage>& image = t->GetImage();
    desc.vkImage = image != nullptr ? uint64_t(image->handle()) : 0;
    tap->onTextureCreate(desc);
}

rt::ResourceId FuseTapContext::textureId(D3D9CommonTexture* t) {
    if (!t) {
        return rt::kNoResource;
    }
    auto it = textures.find(t);
    if (it != textures.end()) {
        return it->second;
    }
    // Created before the tap attached (implicit back buffer, auto depth-stencil): report it now.
    const rt::ResourceId id = nextTexture++;
    textures.emplace(t, id);
    texturesById[id] = t;
    describeTexture(t, id);
    return id;
}

rt::ResourceId FuseTapContext::bufferId(D3D9CommonBuffer* b) {
    if (!b) {
        return rt::kNoResource;
    }
    auto it = buffers.find(b);
    if (it != buffers.end()) {
        return it->second.id;
    }
    const rt::ResourceId id = nextBuffer++;
    buffers[b].id = id;
    const D3D9_BUFFER_DESC* d = b->Desc();
    rt::BufferDesc desc;
    desc.id = id;
    desc.kind = d->Type == D3DRTYPE_INDEXBUFFER ? rt::BufferKind::Index : rt::BufferKind::Vertex;
    desc.size = d->Size;
    desc.usage = d->Usage;
    desc.pool = uint32_t(d->Pool);
    desc.fvf = d->FVF;
    desc.format = uint32_t(d->Format);
    tap->onBufferCreate(desc);
    return id;
}

template <typename T>
rt::ShaderRef FuseTapContext::shaderRef(std::unordered_map<const T*, ShaderEntry<T>>& cache, T* shader) {
    rt::ShaderRef ref;
    if (!shader) {
        return ref;
    }
    auto it = cache.find(shader);
    if (it == cache.end()) {
        ShaderEntry<T> e;
        e.ref = shader;
        e.id = nextShader++;
        UINT size = 0;
        shader->GetFunction(nullptr, &size);
        e.tokens.resize((size + 3) / 4);
        e.byteSize = size;
        if (size) {
            shader->GetFunction(e.tokens.data(), &size);
        }
        it = cache.emplace(shader, std::move(e)).first;
    }
    ref.id = it->second.id;
    ref.tokens = it->second.tokens.data();
    ref.byteSize = it->second.byteSize;
    ref.version = it->second.tokens.empty() ? 0 : it->second.tokens[0];
    return ref;
}

// ---- device ----------------------------------------------------------------------------------------

rt::DeviceEvent FuseTapContext::deviceEvent(D3D9DeviceEx* dev, const D3DPRESENT_PARAMETERS* pp) {
    rt::DeviceEvent e;
    e.adapter = dev->m_adapter ? dev->m_adapter->GetOrdinal() : 0;
    e.deviceType = uint32_t(dev->m_deviceType);
    e.focusWindow = uint64_t(reinterpret_cast<uintptr_t>(dev->m_window));
    e.behaviorFlags = dev->m_behaviorFlags;
    e.extended = dev->m_d3dCompatibility.test(D3DCompatibility::D3D9Ex);
    e.d3d8 = dev->m_d3dCompatibility.test(D3DCompatibility::D3D8);
    if (pp) {
        e.present.backBufferWidth = pp->BackBufferWidth;
        e.present.backBufferHeight = pp->BackBufferHeight;
        e.present.backBufferFormat = uint32_t(pp->BackBufferFormat);
        e.present.backBufferCount = pp->BackBufferCount;
        e.present.multiSampleType = uint32_t(pp->MultiSampleType);
        e.present.multiSampleQuality = pp->MultiSampleQuality;
        e.present.swapEffect = uint32_t(pp->SwapEffect);
        e.present.deviceWindow = uint64_t(reinterpret_cast<uintptr_t>(pp->hDeviceWindow));
        e.present.windowed = pp->Windowed != FALSE;
        e.present.enableAutoDepthStencil = pp->EnableAutoDepthStencil != FALSE;
        e.present.autoDepthStencilFormat = uint32_t(pp->AutoDepthStencilFormat);
        e.present.flags = pp->Flags;
        e.present.fullScreenRefreshRate = pp->FullScreen_RefreshRateInHz;
        e.present.presentationInterval = pp->PresentationInterval;
    }
    if (dev->m_implicitSwapchain != nullptr) {
        D3D9Surface* bb = dev->m_implicitSwapchain->GetBackBuffer(0);
        e.backBuffer = bb ? textureId(bb->GetCommonTexture()) : rt::kNoResource;
    }
    if (dev->m_autoDepthStencil != nullptr) {
        e.autoDepthStencil = textureId(dev->m_autoDepthStencil->GetCommonTexture());
    }
    const Rc<DxvkDevice>& dxvk = dev->m_dxvkDevice;
    e.vulkan.device = uint64_t(reinterpret_cast<uintptr_t>(dxvk->vkd()->device()));
    e.vulkan.physicalDevice = uint64_t(reinterpret_cast<uintptr_t>(dxvk->adapter()->handle()));
    e.vulkan.instance = uint64_t(reinterpret_cast<uintptr_t>(dxvk->instance()->vki()->instance()));
    e.vulkan.queue = uint64_t(reinterpret_cast<uintptr_t>(dxvk->queues().graphics.queueHandle));
    e.vulkan.queueFamily = dxvk->queues().graphics.queueFamily;
    e.vulkan.imported = rt::vkboot::isImportedDevice(e.vulkan.device);
    e.host = this;
    return e;
}

void FuseTap::SwapChainReset(D3D9DeviceEx* dev, const D3DPRESENT_PARAMETERS* pp) {
    if (!dev->m_fuseTap) {
        // Attach (first reset of a device, i.e. InitialReset). With the tap off this stays a
        // cheap no-op on every later reset.
        // createTapForDevice: every mode, including capture (RL-1.2/1.3/1.4 live in-process).
        std::unique_ptr<rt::IRelightTap> tap = rt::createTapForDevice(rt::runtimeConfig(), g_deviceOrdinal.fetch_add(1));
        if (!tap) {
            return;
        }
        auto* ctx = new FuseTapContext();
        ctx->device = dev;
        ctx->tap = std::move(tap);
        dev->m_fuseTap = ctx;
#if defined(FUSE_RELIGHT_HAVE_VERTEX_CAPTURE)
        if (ctx->tap->wantsVertexCapture()) {
            ctx->vertexCapture = FuseVertexCapture::create(dev->m_dxvkDevice, ctx->tap.get());
        }
#endif
        ctx->tap->onDeviceCreate(ctx->deviceEvent(dev, pp));
        return;
    }
    FuseTapContext* ctx = dev->m_fuseTap;
    // Reset runs ResetState, which dirties the lights and clip planes upstream.
    ++ctx->lightsVersion;
    ++ctx->clipPlanesVersion;
    ctx->tap->onDeviceReset(ctx->deviceEvent(dev, pp));
}

void FuseTap::DeviceDestroy(D3D9DeviceEx* dev) {
    FuseTapContext* ctx = dev->m_fuseTap;
    if (!ctx) {
        return;
    }
#if defined(FUSE_RELIGHT_HAVE_VERTEX_CAPTURE)
    ctx->vertexCapture.reset(); // first: stops the SPIR-V offers to the tap
#endif
    ctx->tap->onDeviceDestroy();
    dev->m_fuseTap = nullptr;
    delete ctx;
}

// ---- resources -------------------------------------------------------------------------------------

void FuseTap::TextureCreate(D3D9DeviceEx* dev, D3D9CommonTexture* t) {
    if (FuseTapContext* ctx = dev->m_fuseTap) {
        ctx->textureId(t);
    }
}

void FuseTap::TextureDestroy(D3D9DeviceEx* dev, D3D9CommonTexture* t) {
    FuseTapContext* ctx = dev->m_fuseTap;
    if (!ctx) {
        return;
    }
    auto it = ctx->textures.find(t);
    if (it == ctx->textures.end()) {
        return;
    }
    rt::ImageDestroy d;
    d.texture = it->second;
    const Rc<DxvkImage>& image = t->GetImage();
    d.vkImage = image != nullptr ? uint64_t(image->handle()) : 0;
    ctx->textures.erase(it);
    ctx->texturesById.erase(d.texture);
    ctx->textureDestroyed(t);
    for (auto lock = ctx->locks.begin(); lock != ctx->locks.end();) {
        lock = lock->first.first == t ? ctx->locks.erase(lock) : std::next(lock);
    }
    ctx->tap->onImageDestroy(d);
}

void FuseTap::BufferCreate(D3D9DeviceEx* dev, D3D9CommonBuffer* b) {
    FuseTapContext* ctx = dev->m_fuseTap;
    if (!ctx) {
        return;
    }
    // There is no destruction hook for buffers (patch budget): a new buffer at a known address
    // means the old one is gone.
    auto it = ctx->buffers.find(b);
    if (it != ctx->buffers.end()) {
        const rt::ResourceId old = it->second.id;
        ctx->buffers.erase(it);
        ctx->tap->onBufferDestroy(old);
    }
    ctx->bufferId(b);
}

// ---- uploads ---------------------------------------------------------------------------------------

void FuseTap::TextureLock(D3D9DeviceEx* dev, D3D9CommonTexture* t, UINT face, UINT mip,
                          const D3DLOCKED_BOX* lockedBox, const D3DBOX* box, DWORD flags) {
    FuseTapContext* ctx = dev->m_fuseTap;
    const UINT sub = t->CalcSubresource(face, mip);
    const VkExtent3D extent = t->GetExtentMip(sub);
    TextureLockEntry e;
    e.data = lockedBox->pBits;
    e.rowPitch = uint32_t(lockedBox->RowPitch);
    e.slicePitch = uint32_t(lockedBox->SlicePitch);
    e.flags = flags;
    e.full = box == nullptr;
    if (box) {
        e.box = rt::Box{box->Left, box->Top, box->Right, box->Bottom, box->Front, box->Back};
    } else {
        e.box = rt::Box{0, 0, extent.width, extent.height, 0, extent.depth};
    }
    const uint32_t blockHeight = isBlockCompressed(t->Desc()->Format) ? 4u : 1u;
    e.rows = e.full && e.rowPitch ? e.slicePitch / e.rowPitch
                                  : (e.box.bottom - e.box.top + blockHeight - 1) / blockHeight;
    ctx->locks[{t, sub}] = e;
    if (!(flags & D3DLOCK_READONLY)) {
        rt::TextureWriteLock w;
        w.texture = ctx->textureId(t);
        w.face = face;
        w.level = mip;
        w.lockFlags = flags;
        ctx->tap->onTextureWriteLock(w);
    }
}

void FuseTap::TextureUnlock(D3D9DeviceEx* dev, D3D9CommonTexture* t, UINT face, UINT mip) {
    FuseTapContext* ctx = dev->m_fuseTap;
    const UINT sub = t->CalcSubresource(face, mip);
    auto it = ctx->locks.find({t, sub});
    if (it == ctx->locks.end()) {
        return;
    }
    const TextureLockEntry e = it->second;
    ctx->locks.erase(it);
    if (e.flags & D3DLOCK_READONLY) {
        return;
    }
    ctx->contentChanged(t);
    const VkExtent3D extent = t->GetExtentMip(sub);
    rt::TextureUpload u;
    u.texture = ctx->textureId(t);
    u.face = face;
    u.level = mip;
    u.width = extent.width;
    u.height = extent.height;
    u.depth = extent.depth;
    u.data = e.data;
    u.rowPitch = e.rowPitch;
    u.slicePitch = e.slicePitch;
    u.rows = e.rows;
    u.lockFlags = e.flags;
    u.fullUpdate = e.full;
    u.box = e.box;
    ctx->tap->onTextureUpload(u);
}

void FuseTap::UpdateTexture(D3D9DeviceEx* dev, D3D9CommonTexture* src, D3D9CommonTexture* dst) {
    FuseTapContext* ctx = dev->m_fuseTap;
    rt::TextureCopy c;
    c.method = rt::CopyMethod::UpdateTexture;
    c.source = ctx->textureId(src);
    c.destination = ctx->textureId(dst);
    ctx->contentChanged(dst);
    ctx->tap->onTextureCopy(c);
}

void FuseTap::UpdateSurface(D3D9DeviceEx* dev, IDirect3DSurface9* pSrc, const RECT* srcRect, IDirect3DSurface9* pDst,
                            const POINT* dstPoint) {
    FuseTapContext* ctx = dev->m_fuseTap;
    auto* src = static_cast<D3D9Surface*>(pSrc);
    auto* dst = static_cast<D3D9Surface*>(pDst);
    rt::TextureCopy c;
    c.method = rt::CopyMethod::UpdateSurface;
    c.source = ctx->textureId(src->GetCommonTexture());
    c.destination = ctx->textureId(dst->GetCommonTexture());
    c.sourceFace = src->GetFace();
    c.sourceLevel = src->GetMipLevel();
    c.destFace = dst->GetFace();
    c.destLevel = dst->GetMipLevel();
    c.hasSourceRect = srcRect != nullptr || dstPoint != nullptr;
    // The copied extent: the source rect (validated by UpdateSurface before this hook), else the
    // whole source level.
    if (srcRect) {
        c.width = uint32_t(srcRect->right - srcRect->left);
        c.height = uint32_t(srcRect->bottom - srcRect->top);
    } else {
        D3D9CommonTexture* s = src->GetCommonTexture();
        const VkExtent3D extent = s->GetExtentMip(s->CalcSubresource(src->GetFace(), src->GetMipLevel()));
        c.width = extent.width;
        c.height = extent.height;
    }
    if (dstPoint) {
        c.destX = uint32_t(dstPoint->x);
        c.destY = uint32_t(dstPoint->y);
    }
    ctx->contentChanged(dst->GetCommonTexture());
    ctx->tap->onTextureCopy(c);
}

void FuseTap::BufferLock(D3D9DeviceEx* dev, D3D9CommonBuffer* b, UINT offset, UINT size, DWORD flags) {
    FuseTapContext* ctx = dev->m_fuseTap;
    if (flags & D3DLOCK_READONLY) {
        return;
    }
    const uint32_t total = b->Desc()->Size;
    PendingBufferWrite w;
    w.offset = std::min<uint32_t>(offset, total);
    w.size = size == 0 ? total - w.offset : std::min<uint32_t>(size, total - w.offset);
    w.flags = flags;
    ctx->bufferId(b);
    ctx->buffers[b].pending.push_back(w);
}

void FuseTap::BufferUnlock(D3D9DeviceEx* dev, D3D9CommonBuffer* b) {
    FuseTapContext* ctx = dev->m_fuseTap;
    auto it = ctx->buffers.find(b);
    if (it == ctx->buffers.end() || it->second.pending.empty()) {
        return;
    }
    std::vector<PendingBufferWrite> pending;
    pending.swap(it->second.pending);
    Rc<DxvkResourceAllocation> mapping = b->GetMappedSlice();
    const auto* base = mapping != nullptr ? static_cast<const uint8_t*>(mapping->mapPtr()) : nullptr;
    for (const PendingBufferWrite& p : pending) {
        rt::BufferWrite w;
        w.buffer = it->second.id;
        w.offset = p.offset;
        w.size = p.size;
        w.lockFlags = p.flags;
        w.base = base;
        w.data = base ? base + p.offset : nullptr;
        w.bufferSize = b->Desc()->Size;
        ctx->tap->onBufferWrite(w);
    }
}

void FuseTap::LightsChanged(D3D9DeviceEx* dev) {
    if (FuseTapContext* ctx = dev->m_fuseTap) {
        if (++ctx->lightsVersion == 0) {
            ctx->lightsVersion = 1; // 0 is "not tracked"
        }
    }
}

// ---- draws -----------------------------------------------------------------------------------------

namespace {

uint32_t primitiveVertexCount(D3DPRIMITIVETYPE type, UINT count) {
    switch (type) {
    case D3DPT_POINTLIST:
        return count;
    case D3DPT_LINELIST:
        return count * 2;
    case D3DPT_LINESTRIP:
        return count + 1;
    case D3DPT_TRIANGLELIST:
        return count * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:
        return count + 2;
    default:
        return 0;
    }
}

rt::Color4 color4(const D3DCOLORVALUE& c) { return rt::Color4{c.r, c.g, c.b, c.a}; }
rt::Vec3 vec3(const D3DVECTOR& v) { return rt::Vec3{v.x, v.y, v.z}; }

static_assert(sizeof(Matrix4) == 16 * sizeof(float), "Matrix4 is 16 floats");
static_assert(sizeof(Vector4) == 4 * sizeof(float), "Vector4 is 4 floats");
static_assert(sizeof(Vector4i) == 4 * sizeof(int32_t), "Vector4i is 4 ints");
static_assert(sizeof(D3D9ClipPlane) == 4 * sizeof(float), "clip plane is 4 floats");
static_assert(caps::MaxStreams == rt::kStreamCount, "stream count");
static_assert(caps::MaxTransforms == rt::kTransformCount, "transform count");
static_assert(caps::TextureStageCount == rt::kTextureStageCount, "texture stages");
static_assert(SamplerCount == rt::kSamplerSlotCount, "sampler slots");
static_assert(SamplerStateCount == rt::kSamplerStateCount, "sampler states");
static_assert(TextureStageStateCount == 32, "texture stage states (DXVK_TSS_*)");
static_assert(RenderStateCount == rt::kRenderStateCount, "render states");

} // namespace

bool FuseTap::SkipDraw(D3D9DeviceEx* dev, DrawCall call, D3DPRIMITIVETYPE type, UINT primCount, UINT startVertex,
                       INT baseVertex, UINT minIndex, UINT numVertices, UINT startIndex, const void* pIndexData,
                       D3DFORMAT indexFormat, const void* pVertexData, UINT vertexStride) {
    FuseTapContext* ctx = dev->m_fuseTap;
    Direct3DState9& st = dev->m_state;
    const uint32_t count = primitiveVertexCount(type, primCount);

    rt::DrawCall c;
    c.call = rt::DrawCallType(call);
    c.primitiveType = uint32_t(type);
    c.primitiveCount = primCount;
    const bool indexed = call == DrawIndexed || call == DrawIndexedUP;
    if (indexed) {
        c.baseVertex = baseVertex;
        c.minIndex = minIndex;
        c.numVertices = numVertices;
        c.startIndex = startIndex;
        c.indexCount = count;
        c.instanceCount = dev->GetInstanceCount();
    } else {
        c.startVertex = startVertex;
        c.vertexCount = count;
    }
    if (call == DrawUP || call == DrawIndexedUP) {
        c.upVertexData = pVertexData;
        c.upVertexStride = vertexStride;
        c.upVertexBytes = (indexed ? minIndex + numVertices : count) * vertexStride;
        if (call == DrawIndexedUP) {
            c.upIndexData = pIndexData;
            c.upIndexFormat = uint32_t(indexFormat);
            c.upIndexBytes = count * (indexFormat == D3DFMT_INDEX32 ? 4u : 2u);
        }
    }

    rt::DrawState s;
    static_assert(sizeof(DWORD) == sizeof(uint32_t), "DWORD is 32 bits");
    s.renderStates = reinterpret_cast<const uint32_t*>(st.renderStates.get().data());
    s.textureStageStates = reinterpret_cast<const uint32_t(*)[32]>(st.textureStages.get().data());
    s.samplerStates = reinterpret_cast<const uint32_t(*)[rt::kSamplerStateCount]>(st.samplerStates.get().data());
    for (uint32_t i = 0; i < rt::kSamplerSlotCount; ++i) {
        IDirect3DBaseTexture9* tex = st.textures[i];
        s.textures[i] = tex ? ctx->textureId(GetCommonTexture(tex)) : rt::kNoResource;
    }
    for (uint32_t i = 0; i < rt::kStreamCount; ++i) {
        const D3D9VBO& vbo = st.vertexBuffers[i];
        D3D9CommonBuffer* buf = GetCommonBuffer(vbo.vertexBuffer);
        rt::StreamBinding& b = s.streams[i];
        b.frequency = st.streamFreq[i];
        if (!buf) {
            continue;
        }
        b.buffer = ctx->bufferId(buf);
        b.offset = vbo.offset;
        b.stride = vbo.stride;
        Rc<DxvkResourceAllocation> mapping = buf->GetMappedSlice();
        b.base = mapping != nullptr ? mapping->mapPtr() : nullptr;
        b.bufferSize = buf->Desc()->Size;
    }
    if (D3D9CommonBuffer* ib = GetCommonBuffer(st.indices)) {
        s.indices.buffer = ctx->bufferId(ib);
        s.indices.format = uint32_t(ib->Desc()->Format);
        Rc<DxvkResourceAllocation> mapping = ib->GetMappedSlice();
        s.indices.base = mapping != nullptr ? mapping->mapPtr() : nullptr;
        s.indices.bufferSize = ib->Desc()->Size;
    }
    if (st.vertexDecl != nullptr) {
        const D3D9VertexElements& elems = st.vertexDecl->GetElements();
        for (const D3DVERTEXELEMENT9& e : elems) {
            if (e.Stream == 0xff || s.elementCount >= rt::kMaxVertexElements) {
                break; // D3DDECL_END
            }
            rt::VertexElement& o = s.elements[s.elementCount++];
            o.stream = e.Stream;
            o.offset = e.Offset;
            o.type = e.Type;
            o.method = e.Method;
            o.usage = e.Usage;
            o.usageIndex = e.UsageIndex;
        }
        s.fvf = st.vertexDecl->GetFVF();
    }
    s.vertexShader = ctx->shaderRef(ctx->vertexShaders, st.vertexShader.ptr());
    s.pixelShader = ctx->shaderRef(ctx->pixelShaders, st.pixelShader.ptr());
    s.transforms = reinterpret_cast<const float(*)[16]>(st.transforms.get().data());

    ctx->lights.clear();
    for (uint32_t i = 0; i < st.lights.size(); ++i) {
        const D3D9LightState& ls = st.lights[i];
        if (!ls.isValid) {
            continue;
        }
        rt::Light l;
        l.index = i;
        l.enabled = ls.isEnabled;
        l.type = uint32_t(ls.light.Type);
        l.diffuse = color4(ls.light.Diffuse);
        l.specular = color4(ls.light.Specular);
        l.ambient = color4(ls.light.Ambient);
        l.position = vec3(ls.light.Position);
        l.direction = vec3(ls.light.Direction);
        l.range = ls.light.Range;
        l.falloff = ls.light.Falloff;
        l.attenuation0 = ls.light.Attenuation0;
        l.attenuation1 = ls.light.Attenuation1;
        l.attenuation2 = ls.light.Attenuation2;
        l.theta = ls.light.Theta;
        l.phi = ls.light.Phi;
        ctx->lights.push_back(l);
    }
    s.lights = ctx->lights.data();
    s.lightCount = uint32_t(ctx->lights.size());

    const D3DMATERIAL9& m = st.material.get();
    s.material.diffuse = color4(m.Diffuse);
    s.material.ambient = color4(m.Ambient);
    s.material.specular = color4(m.Specular);
    s.material.emissive = color4(m.Emissive);
    s.material.power = m.Power;
    s.viewport = rt::Viewport{st.viewport.X, st.viewport.Y, st.viewport.Width, st.viewport.Height, st.viewport.MinZ,
                              st.viewport.MaxZ};
    s.scissor = rt::Rect{st.scissorRect.left, st.scissorRect.top, st.scissorRect.right, st.scissorRect.bottom};
    s.clipPlanes = reinterpret_cast<const float(*)[4]>(st.clipPlanes.get().data());
    // DXVK sets its ClipPlanes dirty flag exactly where Remix sets D3D9RtxFlag::DirtyClipPlanes and clears
    // it in the PrepareDraw that follows this hook, so a set flag here means "changed since the last
    // draw". (A draw the tap skipped (Ignore) leaves the flag set: the next draw advances the counter
    // again, which only makes a consumer recompute from unchanged state.)
    if (dev->m_dirty.test(D3D9DeviceDirtyFlag::ClipPlanes) && ++ctx->clipPlanesVersion == 0) {
        ctx->clipPlanesVersion = 1;
    }
    s.lightsVersion = ctx->lightsVersion;
    s.clipPlanesVersion = ctx->clipPlanesVersion;
    s.alphaSwizzleRenderTargets = dev->m_rtSlotTracking.hasAlphaSwizzle;
    s.hasAlphaSwizzleMask = true;
    for (uint32_t i = 0; i < rt::kRenderTargetCount; ++i) {
        s.renderTargets[i] = st.renderTargets[i] != nullptr ? ctx->textureId(st.renderTargets[i]->GetCommonTexture())
                                                            : rt::kNoResource;
    }
    s.depthStencil = st.depthStencil != nullptr ? ctx->textureId(st.depthStencil->GetCommonTexture()) : rt::kNoResource;

    const bool swvp = dev->m_isSWVP;
    s.softwareVertexProcessing = swvp;
    s.vsConstF = reinterpret_cast<const float(*)[4]>(st.vsConsts->fConsts);
    s.vsConstFCount = swvp ? caps::MaxFloatConstantsSoftware : caps::MaxFloatConstantsVS;
    s.vsConstI = reinterpret_cast<const int32_t(*)[4]>(st.vsConsts->iConsts);
    s.vsConstICount = swvp ? caps::MaxOtherConstantsSoftware : caps::MaxOtherConstants;
    s.vsConstB = reinterpret_cast<const uint32_t*>(st.vsConsts->bConsts);
    s.vsConstBCount = swvp ? caps::MaxOtherConstantsSoftware : caps::MaxOtherConstants;
    s.psConstF = reinterpret_cast<const float(*)[4]>(st.psConsts->fConsts);
    s.psConstFCount = caps::MaxSM3FloatConstantsPS;
    s.psConstI = reinterpret_cast<const int32_t(*)[4]>(st.psConsts->iConsts);
    s.psConstICount = caps::MaxOtherConstants;
    s.psConstB = reinterpret_cast<const uint32_t*>(st.psConsts->bConsts);
    s.psConstBCount = caps::MaxOtherConstants;

    const uint64_t drawOrdinal = ctx->drawOrdinal++;
    const bool skip = ctx->tap->onDraw(c, s) == rt::DrawDecision::Ignore;
#if defined(FUSE_RELIGHT_HAVE_VERTEX_CAPTURE)
    // RL-1.6: once capture is on every D3D9 vertex shader has the capture binding (patch RL-1.6-01),
    // so each programmable-VS draw DXVK draws binds its region (or the empty one).
    if (!skip && ctx->vertexCapture && dev->UseProgrammableVS()) {
        dev->EmitCs([cSlice = ctx->vertexCapture->sliceForDraw(drawOrdinal, c)](DxvkContext* cctx) mutable {
            cctx->bindUniformBuffer(VK_SHADER_STAGE_VERTEX_BIT, FuseVertexCapture::kResourceSlot, std::move(cSlice));
        });
    }
#else
    (void)drawOrdinal;
#endif
    return skip;
}

// ---- other events ----------------------------------------------------------------------------------

void FuseTap::Clear(D3D9DeviceEx* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                    DWORD stencil) {
    FuseTapContext* ctx = dev->m_fuseTap;
    Direct3DState9& st = dev->m_state;
    std::vector<rt::Rect> r;
    for (DWORD i = 0; rects && i < count; ++i) {
        r.push_back(rt::Rect{rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2});
    }
    rt::ClearEvent e;
    e.rectCount = uint32_t(r.size());
    e.rects = r.empty() ? nullptr : r.data();
    e.flags = flags;
    e.color = color;
    e.z = z;
    e.stencil = stencil;
    for (uint32_t i = 0; i < rt::kRenderTargetCount; ++i) {
        e.renderTargets[i] = st.renderTargets[i] != nullptr ? ctx->textureId(st.renderTargets[i]->GetCommonTexture())
                                                            : rt::kNoResource;
    }
    e.depthStencil = st.depthStencil != nullptr ? ctx->textureId(st.depthStencil->GetCommonTexture()) : rt::kNoResource;
    ctx->tap->onClear(e);
}

void FuseTap::SetRenderTarget(D3D9DeviceEx* dev, DWORD index, IDirect3DSurface9* surface) {
    FuseTapContext* ctx = dev->m_fuseTap;
    rt::SetRenderTargetEvent e;
    e.index = index;
    e.texture = surface ? ctx->textureId(static_cast<D3D9Surface*>(surface)->GetCommonTexture()) : rt::kNoResource;
    ctx->tap->onSetRenderTarget(e);
}

void FuseTap::Present(D3D9DeviceEx* dev, D3D9SwapChainEx* swapchain) {
    FuseTapContext* ctx = dev->m_fuseTap;
    if (!ctx) {
        return;
    }
    rt::FrameEvent f;
    f.frame = ctx->frame;
    if (D3D9Surface* bb = swapchain->GetBackBuffer(0)) {
        D3D9CommonTexture* t = bb->GetCommonTexture();
        f.backBuffer = ctx->textureId(t);
        const Rc<DxvkImage>& image = t->GetImage();
        f.backBufferVkImage = image != nullptr ? uint64_t(image->handle()) : 0;
        f.width = t->Desc()->Width;
        f.height = t->Desc()->Height;
        f.format = uint32_t(t->Desc()->Format);
    }
#if defined(FUSE_RELIGHT_HAVE_VERTEX_CAPTURE)
    // RL-1.6: the frame's capture regions, once the GPU has written them.
    if (ctx->vertexCapture && ctx->vertexCapture->active()) {
        if (ctx->vertexCapture->pending()) {
            dev->WaitForResource(*ctx->vertexCapture->buffer(), DxvkCsThread::SynchronizeAll, 0);
        }
        ctx->vertexCapture->deliver(*ctx->tap, ctx->frame);
    }
#endif
    // No classifier yet (RL-1.2): the injection point is Present (plan §2.3).
    ctx->tap->onInjectPoint(f);
    ctx->tap->onPresent(f);
    ++ctx->frame;
}

void FuseTap::QueryIssue(D3D9DeviceEx* dev, const void* query, D3DQUERYTYPE type, DWORD issueFlags) {
    FuseTapContext* ctx = dev->m_fuseTap;
    if (!ctx) {
        return;
    }
    std::lock_guard<std::mutex> lock(ctx->queryMutex);
    rt::QueryEvent e;
    e.type = uint32_t(type);
    e.query = uint64_t(reinterpret_cast<uintptr_t>(query));
    if (issueFlags & D3DISSUE_BEGIN) {
        ctx->tap->onQueryBegin(e);
    } else {
        ctx->tap->onQueryEnd(e);
    }
}

// ---- RL-4.1 frame host (frame_host.hpp) --------------------------------------------------------------------
//
// Everything below runs on the application thread inside a tap event (device lock held) and reaches DXVK
// through its own command stream (EmitCs), so FUSE's work is ordered with the application's D3D9 calls.

namespace {

/// A DXVK image as FUSE may create its twin. False for images FUSE cannot mirror.
bool describeImage(const Rc<DxvkImage>& image, rt::HostImageInfo& out) {
    if (image == nullptr) {
        return false;
    }
    const DxvkImageCreateInfo& i = image->info();
    if (i.viewFormatCount > 4 || i.tiling != VK_IMAGE_TILING_OPTIMAL) {
        return false;
    }
    out = rt::HostImageInfo{};
    out.vkImage = uint64_t(image->handle());
    out.imageType = uint32_t(i.type);
    out.format = uint32_t(i.format);
    out.flags = uint32_t(i.flags);
    out.usage = uint32_t(i.usage);
    out.width = i.extent.width;
    out.height = i.extent.height;
    out.depth = i.extent.depth;
    out.mipLevels = i.mipLevels;
    out.arrayLayers = i.numLayers;
    out.samples = uint32_t(i.sampleCount);
    out.aspects = uint32_t(image->formatInfo()->aspectMask);
    out.viewFormatCount = i.viewFormatCount;
    for (uint32_t f = 0; f < i.viewFormatCount; ++f) {
        out.viewFormats[f] = uint32_t(i.viewFormats[f]);
    }
    out.layout = uint32_t(i.layout); // DXVK returns every image to its default layout between commands
    return true;
}

VkImageSubresourceLayers colorLayers(uint32_t mip, uint32_t layers) {
    return VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, mip, 0u, layers};
}

} // namespace

FuseTapContext::~FuseTapContext() {
    // FUSE released its images in onDeviceDestroy (before this); drop whatever the host still holds.
    m_swaps.clear();
    m_images.clear();
    m_acquire = nullptr;
    m_release = nullptr;
}

uint64_t FuseTapContext::getInstanceProcAddr() const {
    return uint64_t(reinterpret_cast<uintptr_t>(device->m_dxvkDevice->instance()->vki()->getLoaderProc()));
}

rt::VulkanDevice FuseTapContext::vulkan() const {
    const Rc<DxvkDevice>& dxvk = device->m_dxvkDevice;
    rt::VulkanDevice v;
    v.device = uint64_t(reinterpret_cast<uintptr_t>(dxvk->vkd()->device()));
    v.physicalDevice = uint64_t(reinterpret_cast<uintptr_t>(dxvk->adapter()->handle()));
    v.instance = uint64_t(reinterpret_cast<uintptr_t>(dxvk->instance()->vki()->instance()));
    v.queue = uint64_t(reinterpret_cast<uintptr_t>(dxvk->queues().graphics.queueHandle));
    v.queueFamily = dxvk->queues().graphics.queueFamily;
    v.imported = rt::vkboot::isImportedDevice(v.device);
    return v;
}

bool FuseTapContext::deviceCreateInfo(rt::HostDeviceInfo& out) const {
    // Only the device FUSE's bootstrap created (RL-1.1 import) has known extensions and features.
    const rt::VulkanDevice v = vulkan();
    if (!v.imported || !rt::vkboot::deviceCreateInfo(v.device, out)) {
        return false;
    }
    out.getInstanceProcAddr = getInstanceProcAddr();
    out.queue = v.queue;
    out.queueFamily = v.queueFamily;
    return true;
}

void FuseTapContext::ensureFences() {
    if (m_acquire == nullptr) {
        DxvkFenceCreateInfo info = {};
        info.initialValue = 0;
        m_acquire = device->m_dxvkDevice->createFence(info);
        m_release = device->m_dxvkDevice->createFence(info);
    }
}

uint64_t FuseTapContext::acquireSemaphore() {
    ensureFences();
    return uint64_t(m_acquire->handle());
}

uint64_t FuseTapContext::releaseSemaphore() {
    ensureFences();
    return uint64_t(m_release->handle());
}

Rc<DxvkImage> FuseTapContext::backBuffer() const {
    if (device->m_implicitSwapchain == nullptr) {
        return nullptr;
    }
    D3D9Surface* bb = device->m_implicitSwapchain->GetBackBuffer(0);
    return bb ? bb->GetCommonTexture()->GetImage() : nullptr;
}

bool FuseTapContext::backBufferInfo(rt::HostImageInfo& out) const { return describeImage(backBuffer(), out); }

bool FuseTapContext::textureInfo(rt::ResourceId texture, rt::HostImageInfo& out) const {
    auto it = texturesById.find(texture);
    return it != texturesById.end() && describeImage(it->second->GetImage(), out);
}

rt::HostImageHandle FuseTapContext::importImage(const rt::FuseImage& image) {
    if (!image.vkImage) {
        return 0;
    }
    const rt::HostImageInfo& f = image.info;
    VkFormat viewFormats[4] = {};
    DxvkImageCreateInfo info = {};
    info.type = VkImageType(f.imageType);
    info.format = VkFormat(f.format);
    info.flags = VkImageCreateFlags(f.flags);
    info.sampleCount = VkSampleCountFlagBits(f.samples ? f.samples : 1u);
    info.extent = VkExtent3D{f.width, f.height, f.depth ? f.depth : 1u};
    info.numLayers = f.arrayLayers ? f.arrayLayers : 1u;
    info.mipLevels = f.mipLevels ? f.mipLevels : 1u;
    info.usage = VkImageUsageFlags(f.usage);
    // FUSE writes these images on its own submissions (transfer), DXVK copies from / into them.
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_GENERAL;        // the hand-over layout (frame_host.hpp)
    info.initialLayout = VK_IMAGE_LAYOUT_GENERAL; // FUSE transitioned it before importing
    info.shared = VK_TRUE;                        // back in GENERAL at the end of every DXVK submission
    info.viewFormatCount = std::min<uint32_t>(f.viewFormatCount, 4u);
    for (uint32_t i = 0; i < info.viewFormatCount; ++i) {
        viewFormats[i] = VkFormat(f.viewFormats[i]);
    }
    info.viewFormats = viewFormats;
    Rc<DxvkImage> dxvkImage =
        device->m_dxvkDevice->importImage(info, VkImage(image.vkImage), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dxvkImage == nullptr) {
        return 0;
    }
    const rt::HostImageHandle handle = m_nextImage++;
    m_images.emplace(handle, std::move(dxvkImage));
    return handle;
}

void FuseTapContext::releaseImage(rt::HostImageHandle image) { m_images.erase(image); }

bool FuseTapContext::copyBackBuffer(rt::HostImageHandle dst) {
    auto it = m_images.find(dst);
    Rc<DxvkImage> bb = backBuffer();
    if (it == m_images.end() || bb == nullptr) {
        return false;
    }
    device->EmitCs([cDst = it->second, cSrc = bb](DxvkContext* c) {
        const VkExtent3D extent = cSrc->mipLevelExtent(0);
        c->copyImage(cDst, colorLayers(0, 1), VkOffset3D{0, 0, 0}, cSrc, colorLayers(0, 1), VkOffset3D{0, 0, 0}, extent);
    });
    return true;
}

bool FuseTapContext::flushAndSignal(uint64_t acquireValue) {
    ensureFences();
    device->EmitCs([cFence = m_acquire, cValue = acquireValue](DxvkContext* c) { c->signalFence(cFence, cValue); });
    // Flush and wait until the command list (ending with the signal) reached the Vulkan queue: FUSE's batch,
    // submitted next under lockQueue, then follows the signal on the queue (no wait-before-signal).
    device->FlushAndSync9On12();
    return true;
}

bool FuseTapContext::composite(rt::HostImageHandle src, uint64_t releaseValue) {
    auto it = m_images.find(src);
    Rc<DxvkImage> bb = backBuffer();
    if (it == m_images.end() || bb == nullptr) {
        return false;
    }
    ensureFences();
    device->EmitCs([cFence = m_release, cValue = releaseValue, cSrc = it->second, cDst = bb](DxvkContext* c) {
        // The command list recorded from here waits (on its first submission) for FUSE's frame.
        c->waitFence(cFence, cValue);
        const VkExtent3D extent = cDst->mipLevelExtent(0);
        c->copyImage(cDst, colorLayers(0, 1), VkOffset3D{0, 0, 0}, cSrc, colorLayers(0, 1), VkOffset3D{0, 0, 0}, extent);
    });
    return true;
}

void FuseTapContext::markBindingsDirty(const D3D9CommonTexture* t) {
    for (uint32_t i = 0; i < SamplerCount; ++i) {
        IDirect3DBaseTexture9* bound = device->m_state.textures[i];
        if (bound != nullptr && GetCommonTexture(bound) == t) {
            device->m_textureSlotTracking.textureDirty |= 1u << i;
        }
    }
}

bool FuseTapContext::setTextureSwap(rt::ResourceId texture, const rt::FuseImage* image) {
    auto it = texturesById.find(texture);
    if (it == texturesById.end()) {
        return false;
    }
    D3D9CommonTexture* t = it->second;
    if (!image) {
        if (m_swaps.erase(t)) {
            markBindingsDirty(t);
        }
        return true;
    }
    const Rc<DxvkImage>& original = t->GetImage();
    rt::HostImageInfo info;
    if (!describeImage(original, info) || info.aspects != VK_IMAGE_ASPECT_COLOR_BIT || info.samples != 1u ||
        image->info.format != info.format || image->info.width != info.width || image->info.height != info.height ||
        image->info.depth != info.depth || image->info.mipLevels != info.mipLevels ||
        image->info.arrayLayers != info.arrayLayers) {
        return false;
    }
    // The twin is the original's twin in every create parameter DXVK looks at (usage from FUSE: a superset);
    // DXVK writes it (the passthrough copy) and lays it out, from UNDEFINED.
    DxvkImageCreateInfo ci = original->info();
    ci.usage = VkImageUsageFlags(image->info.usage);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.shared = VK_FALSE;
    ci.sharing = DxvkSharedHandleInfo();
    Rc<DxvkImage> twin = device->m_dxvkDevice->importImage(ci, VkImage(image->vkImage), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (twin == nullptr) {
        return false;
    }
    TextureSwap swap;
    swap.image = std::move(twin);
    m_swaps[t] = std::move(swap);
    markBindingsDirty(t);
    return true;
}

void FuseTapContext::contentChanged(D3D9CommonTexture* t) {
    auto it = m_swaps.find(t);
    if (it != m_swaps.end()) {
        ++it->second.version;
        markBindingsDirty(t);
    }
}

void FuseTapContext::textureDestroyed(D3D9CommonTexture* t) { m_swaps.erase(t); }

bool FuseTapContext::bindSwapped(DWORD sampler, D3D9CommonTexture* t, bool srgb) {
    auto it = m_swaps.find(t);
    if (it == m_swaps.end()) {
        return false;
    }
    TextureSwap& swap = it->second;
    const Rc<DxvkImage>& original = t->GetImage();
    const Rc<DxvkImageView>& originalView = t->GetSampleView(srgb);
    if (original == nullptr || originalView == nullptr) {
        return false;
    }
    if (swap.copiedVersion != swap.version || swap.copiedFrom != original.ptr()) {
        // Passthrough: the twin takes every subresource of the texture as DXVK holds it now (managed uploads
        // and mip generation are recorded before the bind, in PrepareDraw).
        device->EmitCs([cDst = swap.image, cSrc = original](DxvkContext* c) {
            const DxvkImageCreateInfo& info = cSrc->info();
            for (uint32_t mip = 0; mip < info.mipLevels; ++mip) {
                c->copyImage(cDst, colorLayers(mip, info.numLayers), VkOffset3D{0, 0, 0}, cSrc,
                             colorLayers(mip, info.numLayers), VkOffset3D{0, 0, 0}, cSrc->mipLevelExtent(mip));
            }
        });
        swap.copiedVersion = swap.version;
        swap.copiedFrom = original.ptr();
    }
    // The same view (format, swizzle, type, mip / layer range, layout) on the twin.
    Rc<DxvkImageView> view = swap.image->createView(originalView->info());
    device->EmitCs([cSlot = sampler, cView = std::move(view)](DxvkContext* c) mutable {
        auto [stage, slot] = D3D9ShaderResourceMapping::getTextureSlotInfo(cSlot);
        c->bindResourceImageView(stage, slot, std::move(cView));
    });
    return true;
}

void FuseTapContext::lockQueue() { device->m_dxvkDevice->lockSubmission(); }

void FuseTapContext::unlockQueue() { device->m_dxvkDevice->unlockSubmission(); }

bool FuseTapContext::waitIdle() {
    if (this_thread::isInModuleDetachment()) {
        return false; // DXVK's threads may be gone (see ~D3D9DeviceEx)
    }
    device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    device->m_dxvkDevice->waitForIdle();
    return true;
}

bool FuseTap::BindTexture(D3D9DeviceEx* dev, DWORD sampler, D3D9CommonTexture* t, bool srgb) {
    FuseTapContext* ctx = dev->m_fuseTap;
    return ctx != nullptr && t != nullptr && ctx->bindSwapped(sampler, t, srgb);
}

// ---- the tap's D3D9 name tables match the SDK --------------------------------------------------------
namespace {

constexpr bool fuseNameIs(const rt::names::NameEntry* table, std::size_t n, uint32_t value, std::string_view name) {
    for (std::size_t i = 0; i < n; ++i) {
        if (table[i].value == value) {
            return std::string_view(table[i].name) == name;
        }
    }
    return false;
}
template <std::size_t N>
constexpr bool fuseNameIs(const rt::names::NameEntry (&table)[N], uint32_t value, std::string_view name) {
    return fuseNameIs(table, N, value, name);
}
namespace names = rt::names;

#include "fuse_tap_names_check.inc"

} // namespace

} // namespace dxvk
