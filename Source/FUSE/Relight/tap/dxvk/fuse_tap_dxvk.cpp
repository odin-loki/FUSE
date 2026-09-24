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
#include <fuse/relight/tap/relight_tap.hpp>
#include <fuse/relight/tap/tap_config.hpp>
#include <fuse/relight/tap/vk_bootstrap.hpp>

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

/// Per-device dispatcher state (D3D9DeviceEx::m_fuseTap).
class FuseTapContext {
public:
    std::unique_ptr<rt::IRelightTap> tap;
    std::mutex queryMutex;
    std::unordered_map<const D3D9CommonTexture*, rt::ResourceId> textures;
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

    rt::ResourceId textureId(D3D9CommonTexture* t);
    rt::ResourceId bufferId(D3D9CommonBuffer* b);
    void describeTexture(D3D9CommonTexture* t, rt::ResourceId id);
    rt::DeviceEvent deviceEvent(D3D9DeviceEx* dev, const D3DPRESENT_PARAMETERS* pp);
    template <typename T>
    rt::ShaderRef shaderRef(std::unordered_map<const T*, ShaderEntry<T>>& cache, T* shader);
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
        ctx->tap = std::move(tap);
        dev->m_fuseTap = ctx;
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

    return ctx->tap->onDraw(c, s) == rt::DrawDecision::Ignore;
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
