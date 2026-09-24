// FUSE Relight RL-1.4: texture tracking and Remix-compatible texture hashing (see
// texture_tracker.hpp).
//
// The hashing rules restate NV-DXVK code of dxvk-remix @0867d3c (src/d3d9/d3d9_common_texture.cpp:
// CreatePrimaryImage, SetupForRtxFrom, SetupForRtx, ClearHash; src/d3d9/d3d9_device.cpp:
// UpdateSurface, UpdateTexture, LockImage, UnlockImage, FlushImage, UploadManagedTexture,
// PrepareTextures). Those NV-DXVK sections are under the MIT licence:
//
//     Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
//
//     Permission is hereby granted, free of charge, to any person obtaining a copy of this
//     software and associated documentation files (the "Software"), to deal in the Software
//     without restriction, including without limitation the rights to use, copy, modify, merge,
//     publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
//     to whom the Software is furnished to do so, subject to the following conditions:
//
//     The above copyright notice and this permission notice shall be included in all copies or
//     substantial portions of the Software.
//
//     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
//     INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
//     PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
//     FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
//     OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//     DEALINGS IN THE SOFTWARE.
//
// Modifications Copyright (c) 2026 FUSE contributors (MIT): the rules are re-expressed over the
// FUSE Relight tap events (no DXVK objects); the staging buffer is FUSE's canonical mip-0 shadow.
#include <fuse/relight/capture/texture/texture_tracker.hpp>

#include <algorithm>
#include <utility>

namespace fuse::relight::capture::texture {

namespace {

// D3D9 values (public API constants).
constexpr std::uint32_t kPoolDefault = 0;
constexpr std::uint32_t kPoolManaged = 1;
constexpr std::uint32_t kPoolSystemMem = 2;
constexpr std::uint32_t kPoolScratch = 3;
constexpr std::uint32_t kUsageRenderTarget = hash::kD3DUsageRenderTarget;
constexpr std::uint32_t kTypeSurface = std::uint32_t(hash::D3DResourceType::Surface);
constexpr std::uint32_t kTypeTexture = std::uint32_t(hash::D3DResourceType::Texture);
constexpr std::uint32_t kFormatNull = hash::makeFourCC('N', 'U', 'L', 'L');
constexpr std::uint32_t kTssColorOpIndex = 0; // D3DTSS_COLOROP (1) - 1, DXVK's stage-state layout
constexpr std::uint32_t kTopDisable = 1;      // D3DTOP_DISABLE
constexpr std::uint32_t kPixelSamplerSlots = 16;

std::uint32_t mipExtent(std::uint32_t v, std::uint32_t level) {
    return std::max(1u, level < 32 ? v >> level : 0u);
}

hash::TextureDescriptor descriptorOf(const tap::TextureDesc& d, std::uint32_t multisampleQuality) {
    hash::TextureDescriptor out;
    out.width = d.width;
    out.height = d.height;
    out.depth = d.depth;
    out.arraySize = d.arraySize;
    out.mipLevels = d.mipLevels;
    out.usage = d.usage;
    out.format = d.format;
    out.pool = d.pool;
    out.multiSample = d.multiSample;
    out.multisampleQuality = multisampleQuality;
    out.discard = false; // D3D9 sets Discard only on depth-stencil surfaces, which are not render targets
    out.isBackBuffer = d.isBackBuffer;
    out.isAttachmentOnly = d.isAttachmentOnly;
    return out;
}

} // namespace

std::atomic<std::uint32_t>& processRenderTargetHashCounter() {
    static std::atomic<std::uint32_t> counter{0};
    return counter;
}

const char* hashOriginName(HashOrigin origin) {
    switch (origin) {
    case HashOrigin::None: return "none";
    case HashOrigin::Upload: return "upload";
    case HashOrigin::Inherited: return "inherited";
    case HashOrigin::RenderTarget: return "render_target";
    }
    return "?";
}

struct TextureTracker::Entry {
    tap::TextureDesc desc;
    bool backed = false;      ///< DXVK map mode BACKED: the texture has an image (and a hash slot)
    bool hashable = false;    ///< SetupForRtxFrom applies: D3DRTYPE_TEXTURE, not depth-stencil
    bool systemMem = false;   ///< D3DPOOL_SYSTEMMEM: its buffers exist from creation
    bool trackMip0 = false;   ///< keep the canonical subresource-0 buffer
    bool managed = false;
    bool needsUpload0 = false; ///< DXVK NeedsUpload(subresource 0)
    std::uint32_t multisampleQuality = 0;
    CanonicalMip0 mip0;
    hash::Hash64 imageHash = hash::kEmptyHash;
    hash::Hash64 descriptorHash = hash::kEmptyHash;
    HashOrigin origin = HashOrigin::None;
    tap::ResourceId inheritedFrom = tap::kNoResource;
    bool obsolete = false;
    std::uint32_t hashCount = 0;
    std::uint32_t clearCount = 0;
    ExternalImageHandle image;
    bool mip0Locked = false; ///< subresource 0 has been locked (its staging buffer was created)

    TrackedTexture snapshot() const {
        TrackedTexture t;
        t.desc = desc;
        t.imageHash = imageHash;
        t.descriptorHash = descriptorHash;
        t.origin = origin;
        t.inheritedFrom = inheritedFrom;
        t.obsoleteHash = obsolete;
        t.hashable = backed && hashable;
        t.flushPending = managed && needsUpload0 && mip0Locked && t.hashable && imageHash == hash::kEmptyHash;
        t.hashCount = hashCount;
        t.clearCount = clearCount;
        t.image = image;
        return t;
    }
};

TextureTracker::TextureTracker(TextureTrackerConfig config, ExternalImageRegistry* registry)
    : m_config(std::move(config)), m_registry(registry) {
    m_retainShadow = m_config.retainShadowAfterHash.value_or(m_config.recomputeTextureHashOnWrite);
    m_rtCounter = m_config.renderTargetCounter ? m_config.renderTargetCounter : &processRenderTargetHashCounter();
}

TextureTracker::~TextureTracker() = default;

TextureTracker::Entry* TextureTracker::findLocked(tap::ResourceId id) {
    auto it = m_textures.find(id);
    return it == m_textures.end() ? nullptr : it->second.get();
}

const TextureTracker::Entry* TextureTracker::findLocked(tap::ResourceId id) const {
    auto it = m_textures.find(id);
    return it == m_textures.end() ? nullptr : it->second.get();
}

// ---- device --------------------------------------------------------------------------------------

void TextureTracker::refreshBackBufferDescriptorLocked(const tap::DeviceEvent& e) {
    // TextureDesc carries no MultisampleQuality; the swap chain's comes with the device event
    // (the back buffer is created, and its descriptor hashed, just before onDeviceCreate).
    Entry* bb = findLocked(e.backBuffer);
    if (!bb || bb->multisampleQuality == e.present.multiSampleQuality) {
        return;
    }
    bb->multisampleQuality = e.present.multiSampleQuality;
    if (bb->descriptorHash != hash::kEmptyHash) {
        bb->descriptorHash = hash::hashTextureDescriptor(descriptorOf(bb->desc, bb->multisampleQuality));
        syncRegistryLocked(*bb);
    }
}

void TextureTracker::onDeviceCreate(const tap::DeviceEvent& e) {
    std::lock_guard<std::mutex> lock(m_mutex);
    refreshBackBufferDescriptorLocked(e);
}

void TextureTracker::onDeviceReset(const tap::DeviceEvent& e) {
    std::lock_guard<std::mutex> lock(m_mutex);
    refreshBackBufferDescriptorLocked(e);
}

void TextureTracker::onDeviceDestroy() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_textures.clear();
    if (m_registry) {
        m_registry->releaseAll();
    }
}

// ---- textures ------------------------------------------------------------------------------------

void TextureTracker::onTextureCreate(const tap::TextureDesc& desc) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_textures.count(desc.id) && m_registry) {
        m_registry->release(desc.id);
    }
    auto e = std::make_unique<Entry>();
    e->desc = desc;
    // DetermineMapMode: NULL format -> no image; SYSTEMMEM / SCRATCH -> CPU buffers only.
    e->systemMem = desc.pool == kPoolSystemMem;
    e->backed = desc.format != kFormatNull && desc.pool != kPoolSystemMem && desc.pool != kPoolScratch;
    e->hashable = hash::isTextureHashed(hash::D3DResourceType(desc.type), desc.usage);
    e->managed = desc.pool == kPoolManaged;
    e->needsUpload0 = desc.pool != kPoolDefault; // constructor: every subresource of a non-DEFAULT pool
    // Subresource 0 is kept for hashable images and for SYSTEMMEM surfaces / textures (the
    // sources SetupForRtxFrom reads on UpdateTexture / UpdateSurface).
    e->trackMip0 = (e->backed && e->hashable) || (e->systemMem && (desc.type == kTypeTexture || desc.type == kTypeSurface));
    if (e->trackMip0) {
        e->mip0 = CanonicalMip0(hash::D3DFormat(desc.format), desc.width, desc.height, m_config.formatTable);
    }
    if (e->backed && (desc.usage & kUsageRenderTarget)) {
        // CreatePrimaryImage: render targets are hashed from their extent and a creation counter.
        const std::uint32_t extent[3] = {desc.width, desc.height, std::max(1u, desc.depth)};
        const std::uint32_t counter = m_rtCounter->fetch_add(1, std::memory_order_relaxed);
        const hash::Hash64 extentHash = hash::xxh3_64(extent, sizeof extent);
        e->imageHash = hash::xxh3_64(&counter, sizeof counter, extentHash);
        e->descriptorHash = hash::hashTextureDescriptor(descriptorOf(desc, e->multisampleQuality));
        e->origin = HashOrigin::RenderTarget;
        e->hashCount = 1;
    }
    Entry& ref = *e;
    m_textures[desc.id] = std::move(e);
    if (m_registry && desc.vkImage != 0) {
        ExternalImageInfo info;
        info.texture = desc.id;
        info.vkImage = desc.vkImage;
        info.type = desc.type;
        info.format = desc.format;
        info.width = desc.width;
        info.height = desc.height;
        info.depth = desc.depth;
        info.mipLevels = desc.mipLevels;
        info.arraySize = desc.arraySize;
        info.usage = desc.usage;
        info.pool = desc.pool;
        info.imageHash = ref.imageHash;
        info.descriptorHash = ref.descriptorHash;
        ref.image = m_registry->registerImage(info);
    }
}

void TextureTracker::onTextureWriteLock(const tap::TextureWriteLock& l) {
    std::lock_guard<std::mutex> lock(m_mutex);
    Entry* e = findLocked(l.texture);
    if (!e) {
        ++m_stats.unknownTexture;
        return;
    }
    const bool sub0 = l.face == 0 && l.level == 0;
    if (sub0) {
        // CreateBufferSubresource: the lock creates the staging buffer.
        e->mip0Locked = true;
        if (e->trackMip0 && shadowWanted(*e)) {
            e->mip0.allocate();
        }
    }
    // LockImage (NV-DXVK): recompute-on-write drops the hash of a written mip 0, unless a texture
    // list pins it. Any face: MipLevel == 0.
    if (l.level == 0 && m_config.recomputeTextureHashOnWrite && e->backed && e->imageHash != hash::kEmptyHash) {
        const bool keep = m_config.keepHashOnWrite && m_config.keepHashOnWrite(e->imageHash);
        if (!keep) {
            clearHashLocked(*e);
        }
    }
    if (sub0 && e->managed && !m_config.evictManagedOnUnlock) {
        e->needsUpload0 = true;
    }
}

void TextureTracker::onTextureUpload(const tap::TextureUpload& u) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_stats.uploads;
    Entry* e = findLocked(u.texture);
    if (!e) {
        ++m_stats.unknownTexture;
        return;
    }
    if (u.face != 0 || u.level != 0) {
        return; // only subresource 0 is hashed; other subresources flush without SetupForRtx
    }
    e->mip0Locked = true;
    // The dirty box: a full lock, or a non-empty rectangle (AddDirtyBox ignores empty boxes).
    const bool dirty = u.fullUpdate || (u.box.right > u.box.left && u.box.bottom > u.box.top);
    if (e->trackMip0 && shadowWanted(*e)) {
        if (u.fullUpdate) {
            e->mip0.writeFull(u.data, u.rowPitch, u.rows);
        } else {
            e->mip0.writeRect(u.data, u.rowPitch, TexelRect{u.box.left, u.box.top, u.box.right, u.box.bottom});
        }
        ++m_stats.mip0Writes;
    }
    if (!e->backed) {
        return;
    }
    if (e->managed && !m_config.evictManagedOnUnlock) {
        e->needsUpload0 = true; // uploaded (and hashed) when a draw first samples it
        return;
    }
    // UnlockImage: DEFAULT (and managed with evictManagedOnUnlock) flush a dirty subresource now.
    if (dirty) {
        flushSubresource0Locked(*e);
    }
}

void TextureTracker::onTextureCopy(const tap::TextureCopy& c) {
    std::lock_guard<std::mutex> lock(m_mutex);
    Entry* src = findLocked(c.source);
    Entry* dst = findLocked(c.destination);
    if (!src || !dst) {
        ++m_stats.unknownTexture;
        return;
    }
    if (c.method == tap::CopyMethod::UpdateTexture) {
        setupForRtxFromLocked(*dst, *src);
        return;
    }
    // UpdateSurface: only a copy covering the destination image's (mip-0) extent inherits.
    bool full = false;
    if (!c.hasSourceRect || m_config.sourceRectPolicy == SourceRectPolicy::AssumeFullWhenExtentsMatch) {
        full = mipExtent(src->desc.width, c.sourceLevel) == std::max(1u, dst->desc.width) &&
               mipExtent(src->desc.height, c.sourceLevel) == std::max(1u, dst->desc.height) &&
               std::max(1u, dst->desc.depth) == 1;
    }
    if (full) {
        setupForRtxFromLocked(*dst, *src);
    }
}

void TextureTracker::onImageDestroy(const tap::ImageDestroy& d) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_textures.find(d.texture);
    if (it == m_textures.end()) {
        ++m_stats.unknownTexture;
        return;
    }
    if (m_registry) {
        m_registry->release(d.texture);
    }
    m_textures.erase(it);
}

tap::DrawDecision TextureTracker::onDraw(const tap::DrawCall&, const tap::DrawState& state) {
    flushForDraw(state);
    return tap::DrawDecision::Raster;
}

// ---- hashing -------------------------------------------------------------------------------------

void TextureTracker::setupForRtxFromLocked(Entry& dst, Entry& src) {
    // SetupForRtxFrom(source)
    if (!dst.backed || !dst.hashable || dst.imageHash != hash::kEmptyHash) {
        return;
    }
    // "Data may not be there yet": the source's subresource-0 buffer must exist.
    if (!src.trackMip0 || !(src.systemMem || src.mip0Locked)) {
        return;
    }
    const bool obsolete = dst.needsUpload0 && m_config.useObsoleteHashOnTextureUpload;
    const hash::Hash64 h = src.mip0.hash(obsolete);
    m_stats.bytesHashed += src.mip0.bytes().size();
    ++m_stats.hashesComputed;
    if (&src != &dst) {
        ++m_stats.inherited;
    }
    setImageHashLocked(dst, h, &src == &dst ? HashOrigin::Upload : HashOrigin::Inherited,
                       &src == &dst ? tap::kNoResource : src.desc.id, obsolete);
    maybeReleaseShadowLocked(src);
}

void TextureTracker::flushSubresource0Locked(Entry& e) {
    // FlushImage(subresource 0) -> SetupForRtx() = SetupForRtxFrom(this)
    setupForRtxFromLocked(e, e);
}

void TextureTracker::setImageHashLocked(Entry& e, hash::Hash64 image, HashOrigin origin, tap::ResourceId from,
                                        bool obsolete) {
    e.imageHash = image;
    e.origin = origin;
    e.inheritedFrom = from;
    e.obsolete = obsolete;
    ++e.hashCount;
    if (e.desc.usage & kUsageRenderTarget) {
        e.descriptorHash = hash::hashTextureDescriptor(descriptorOf(e.desc, e.multisampleQuality));
    }
    syncRegistryLocked(e);
    maybeReleaseShadowLocked(e);
}

void TextureTracker::clearHashLocked(Entry& e) {
    e.imageHash = hash::kEmptyHash;
    e.descriptorHash = hash::kEmptyHash;
    e.origin = HashOrigin::None;
    e.inheritedFrom = tap::kNoResource;
    e.obsolete = false;
    ++e.clearCount;
    ++m_stats.cleared;
    syncRegistryLocked(e);
}

void TextureTracker::syncRegistryLocked(const Entry& e) {
    if (m_registry && e.image.valid()) {
        m_registry->updateHashes(e.desc.id, e.imageHash, e.descriptorHash);
    }
}

bool TextureTracker::shadowWanted(const Entry& e) const {
    // Once hashed, a texture's writes matter only if its hash can be cleared (recompute-on-write)
    // or it is a SYSTEMMEM source.
    return e.systemMem || m_retainShadow || e.imageHash == hash::kEmptyHash;
}

void TextureTracker::maybeReleaseShadowLocked(Entry& e) {
    // SYSTEMMEM sources stay: every later UpdateTexture / UpdateSurface reads them again.
    if (!m_retainShadow && !e.systemMem && e.imageHash != hash::kEmptyHash) {
        e.mip0.release();
    }
}

// ---- managed uploads -----------------------------------------------------------------------------

void TextureTracker::flushSlotLocked(tap::ResourceId id) {
    if (id == tap::kNoResource) {
        return;
    }
    Entry* e = findLocked(id);
    if (!e || !e->managed) {
        return;
    }
    // UploadManagedTexture: flush every subresource that needs upload and has a buffer, then
    // ClearNeedsUpload().
    if (e->needsUpload0 && e->mip0Locked) {
        flushSubresource0Locked(*e);
    }
    e->needsUpload0 = false;
}

void TextureTracker::flushForDraw(const tap::DrawState& state) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (state.pixelShader.id == tap::kNoResource) {
        // Fixed-function pixel stages: up to the first disabled colour op.
        for (std::uint32_t stage = 0; stage < tap::kTextureStageCount; ++stage) {
            if (state.textureStageStates && state.textureStageStates[stage][kTssColorOpIndex] == kTopDisable) {
                break;
            }
            flushSlotLocked(state.textures[stage]);
        }
    } else {
        for (std::uint32_t slot = 0; slot < kPixelSamplerSlots; ++slot) {
            flushSlotLocked(state.textures[slot]);
        }
    }
    if (state.vertexShader.id != tap::kNoResource) {
        for (std::uint32_t slot = kPixelSamplerSlots; slot < tap::kSamplerSlotCount; ++slot) {
            flushSlotLocked(state.textures[slot]);
        }
    }
}

void TextureTracker::flushManaged(tap::ResourceId texture) {
    std::lock_guard<std::mutex> lock(m_mutex);
    flushSlotLocked(texture);
}

void TextureTracker::flushAllPending() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, e] : m_textures) {
        if (e->managed && e->needsUpload0 && e->mip0Locked) {
            flushSlotLocked(id);
        }
    }
}

// ---- queries -------------------------------------------------------------------------------------

std::optional<TrackedTexture> TextureTracker::find(tap::ResourceId texture) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const Entry* e = findLocked(texture);
    if (!e) {
        return std::nullopt;
    }
    return e->snapshot();
}

hash::Hash64 TextureTracker::imageHash(tap::ResourceId texture) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const Entry* e = findLocked(texture);
    return e ? e->imageHash : hash::kEmptyHash;
}

hash::Hash64 TextureTracker::descriptorHash(tap::ResourceId texture) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const Entry* e = findLocked(texture);
    return e ? e->descriptorHash : hash::kEmptyHash;
}

hash::Hash64 TextureTracker::previewImageHash(tap::ResourceId texture) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const Entry* e = findLocked(texture);
    if (!e) {
        return hash::kEmptyHash;
    }
    if (e->imageHash != hash::kEmptyHash) {
        return e->imageHash;
    }
    if (!e->trackMip0 || !(e->systemMem || e->mip0Locked)) {
        return hash::kEmptyHash;
    }
    const bool obsolete = e->needsUpload0 && m_config.useObsoleteHashOnTextureUpload;
    const std::vector<std::uint8_t>& bytes = e->mip0.bytes();
    if (bytes.empty() && e->mip0.layout().size != 0) {
        const std::vector<std::uint8_t> zeros(std::size_t(e->mip0.layout().size), 0);
        return obsolete ? hash::hashTextureMip0Obsolete(zeros.data(), zeros.size())
                        : hash::hashTextureMip0(zeros.data(), zeros.size());
    }
    return obsolete ? hash::hashTextureMip0Obsolete(bytes.data(), bytes.size())
                    : hash::hashTextureMip0(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> TextureTracker::mip0Bytes(tap::ResourceId texture) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const Entry* e = findLocked(texture);
    return e ? e->mip0.bytes() : std::vector<std::uint8_t>();
}

std::vector<TrackedTexture> TextureTracker::textures() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<TrackedTexture> out;
    out.reserve(m_textures.size());
    for (const auto& [id, e] : m_textures) {
        out.push_back(e->snapshot());
    }
    return out;
}

std::size_t TextureTracker::liveCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_textures.size();
}

TextureTrackerStats TextureTracker::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

} // namespace fuse::relight::capture::texture
