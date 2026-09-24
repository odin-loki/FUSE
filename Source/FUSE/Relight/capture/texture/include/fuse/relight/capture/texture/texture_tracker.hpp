// FUSE Relight RL-1.4: texture tracking and Remix-compatible texture hashing on upload
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1.3, Wave R1).
//
// TextureTracker consumes the tap's texture events (RL-1.1, IRelightTap) and reproduces when and
// over what NV-DXVK (dxvk-remix @0867d3c) sets a texture's image hash and render-target descriptor
// hash:
//
//  * Which textures: the hash lives on the DXVK image, so only textures with one (the pool is not
//    D3DPOOL_SYSTEMMEM / SCRATCH, the format is not NULL) are hashed at all. Content hashes are
//    set only on D3DRTYPE_TEXTURE without D3DUSAGE_DEPTHSTENCIL (SetupForRtxFrom).
//  * First upload (SetupForRtx, from FlushImage of subresource 0 = face 0, mip 0): the hash is
//    XXH3_64bits over the whole mip-0 staging buffer in the canonical packed layout
//    (CanonicalMip0), set once while it is empty. D3DPOOL_DEFAULT textures flush when the lock
//    is released; D3DPOOL_MANAGED textures (d3d9.evictManagedOnUnlock off, DXVK's default) flush
//    when a draw first samples them, so every write made before that draw is part of the hash.
//  * rtx.useObsoleteHashOnTextureUpload: XXH64(data, size, 0) instead, when the flushed
//    subresource still "needs upload" (managed textures; never D3DPOOL_DEFAULT ones).
//  * UpdateTexture / full-size UpdateSurface (SYSTEMMEM -> DEFAULT): the destination inherits the
//    hash of the *source's* subresource-0 buffer (SetupForRtxFrom(source)), when it has none yet.
//    UpdateSurface inherits only when the copied extent equals the destination's mip-0 extent.
//  * rtx.recomputeTextureHashOnWrite: a write lock of mip 0 (any face) clears the image and
//    descriptor hashes (ClearHash), unless the hash is listed in rtx.terrainTextures,
//    rtx.lightmapTextures, rtx.ignoreTextures or rtx.ignoreBakedLightingTextures; the next flush
//    of subresource 0 hashes again.
//  * Render targets (D3DUSAGE_RENDERTARGET, any resource type, including swap-chain back
//    buffers) get, at image creation, image hash = XXH3_64bits_withSeed(&counter, 4,
//    XXH3_64bits(&extent, 12)) with a process-wide creation counter, and descriptor hash =
//    D3D9_COMMON_TEXTURE_DESC::CalculateHash ("remix.rtdesc"). A render target that is hashed
//    again after ClearHash also gets its descriptor hash recomputed.
//
// Every live texture with a VkImage is registered in an ExternalImageRegistry (if one is given)
// and released on onImageDestroy; its hashes follow every change.
//
// Thread-safe (one mutex): the tap delivers events on the application's thread, consumers query
// from anywhere. TextureTracker is an IRelightTap, so it can be driven directly by the tap or by a
// composite tap that forwards texture events and draws.
#pragma once

#include <fuse/relight/capture/texture/canonical_mip0.hpp>
#include <fuse/relight/capture/texture/external_image_registry.hpp>
#include <fuse/relight/hash/texture_hash.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace fuse::relight::capture::texture {

/// The process-wide counter NV-DXVK seeds render-target image hashes with (a function-local static
/// in D3D9CommonTexture::CreatePrimaryImage). Tests pass their own.
std::atomic<std::uint32_t>& processRenderTargetHashCounter();

/// How a UpdateSurface with a source rectangle or destination point is judged, since the tap
/// reports only that one was given (TextureCopy::hasSourceRect), not its extent. DXVK's d3d8
/// CopyRects always passes a rectangle (the whole surface when the application passes none).
enum class SourceRectPolicy : std::uint8_t {
    AssumeFullWhenExtentsMatch, ///< inherit when the source level's extent equals the destination's mip 0
    NeverInherit,               ///< treat every rectangle copy as partial
};

struct TextureTrackerConfig {
    bool useObsoleteHashOnTextureUpload = false; ///< rtx.useObsoleteHashOnTextureUpload
    bool recomputeTextureHashOnWrite = false;    ///< rtx.recomputeTextureHashOnWrite
    bool evictManagedOnUnlock = false;           ///< d3d9.evictManagedOnUnlock (DXVK default false)
    /// Keep the canonical mip-0 buffer after a texture is hashed. Needed only when a later write
    /// can matter (recompute-on-write, or partial writes after ClearHash); SYSTEMMEM sources keep
    /// theirs regardless. Defaults to recomputeTextureHashOnWrite when unset.
    std::optional<bool> retainShadowAfterHash;
    hash::FormatTableOptions formatTable;
    SourceRectPolicy sourceRectPolicy = SourceRectPolicy::AssumeFullWhenExtentsMatch;
    /// The terrain / lightmap / ignore / ignoreBakedLighting texture lists: true keeps the hash on
    /// a mip-0 write lock. Null: no list configured.
    std::function<bool(hash::Hash64)> keepHashOnWrite;
    /// Render-target hash counter; null = processRenderTargetHashCounter().
    std::atomic<std::uint32_t>* renderTargetCounter = nullptr;
};

/// How a texture's current image hash came about.
enum class HashOrigin : std::uint8_t {
    None = 0,
    Upload,       ///< SetupForRtx: first flush of subresource 0
    Inherited,    ///< UpdateTexture / UpdateSurface from `inheritedFrom`
    RenderTarget, ///< render-target creation counter
};

const char* hashOriginName(HashOrigin origin);

/// A snapshot of one tracked texture.
struct TrackedTexture {
    tap::TextureDesc desc;
    hash::Hash64 imageHash = hash::kEmptyHash;
    hash::Hash64 descriptorHash = hash::kEmptyHash;
    HashOrigin origin = HashOrigin::None;
    tap::ResourceId inheritedFrom = tap::kNoResource;
    bool obsoleteHash = false;    ///< imageHash is XXH64 (rtx.useObsoleteHashOnTextureUpload)
    bool hashable = false;        ///< content hashes apply (TEXTURE, not depth, has an image)
    bool flushPending = false;    ///< managed: mip 0 written, hashed at the first sampling draw
    std::uint32_t hashCount = 0;  ///< times an image hash was set (re-hash after ClearHash counts)
    std::uint32_t clearCount = 0; ///< recompute-on-write clears
    ExternalImageHandle image;    ///< registry handle (invalid without a registry or VkImage)
};

struct TextureTrackerStats {
    std::uint64_t uploads = 0;       ///< texture_upload events seen
    std::uint64_t mip0Writes = 0;    ///< of which re-packed into a canonical mip-0 buffer
    std::uint64_t hashesComputed = 0;
    std::uint64_t bytesHashed = 0;
    std::uint64_t inherited = 0;
    std::uint64_t cleared = 0;
    std::uint64_t unknownTexture = 0; ///< events naming a texture never created
};

class TextureTracker final : public tap::IRelightTap {
public:
    explicit TextureTracker(TextureTrackerConfig config = {}, ExternalImageRegistry* registry = nullptr);
    ~TextureTracker() override;
    TextureTracker(const TextureTracker&) = delete;
    TextureTracker& operator=(const TextureTracker&) = delete;

    // ---- IRelightTap (texture events, device lifetime, draws) ---------------------------------
    void onDeviceCreate(const tap::DeviceEvent& e) override;
    void onDeviceReset(const tap::DeviceEvent& e) override;
    void onDeviceDestroy() override;
    void onTextureCreate(const tap::TextureDesc& desc) override;
    void onTextureUpload(const tap::TextureUpload& upload) override;
    void onTextureCopy(const tap::TextureCopy& copy) override;
    void onTextureWriteLock(const tap::TextureWriteLock& lock) override;
    void onImageDestroy(const tap::ImageDestroy& destroy) override;
    /// Flushes the managed textures the draw samples (flushForDraw) and returns Raster.
    tap::DrawDecision onDraw(const tap::DrawCall& call, const tap::DrawState& state) override;

    // ---- explicit flushes ---------------------------------------------------------------------
    /// DXVK PrepareTextures: managed textures bound to samplers the draw uses are uploaded, which
    /// hashes a pending subresource 0. Fixed-function pixel stages count up to the first stage with
    /// D3DTSS_COLOROP = D3DTOP_DISABLE; a pixel shader uses every bound pixel sampler, a vertex
    /// shader every bound vertex sampler.
    void flushForDraw(const tap::DrawState& state);
    /// UploadManagedTexture for one texture (PreLoad, or a consumer that knows it was sampled).
    void flushManaged(tap::ResourceId texture);
    /// Flushes every pending managed texture (captures, tests).
    void flushAllPending();

    // ---- queries ------------------------------------------------------------------------------
    std::optional<TrackedTexture> find(tap::ResourceId texture) const;
    hash::Hash64 imageHash(tap::ResourceId texture) const;
    hash::Hash64 descriptorHash(tap::ResourceId texture) const;
    /// What the image hash would be if subresource 0 were flushed now (the hash when set; else the
    /// hash of the current canonical buffer when it exists; else kEmptyHash). For diagnostics.
    hash::Hash64 previewImageHash(tap::ResourceId texture) const;
    /// A copy of the canonical mip-0 buffer (empty when none is held).
    std::vector<std::uint8_t> mip0Bytes(tap::ResourceId texture) const;
    /// Live textures, ascending id.
    std::vector<TrackedTexture> textures() const;
    std::size_t liveCount() const;
    TextureTrackerStats stats() const;
    const TextureTrackerConfig& config() const { return m_config; }

private:
    struct Entry;

    Entry* findLocked(tap::ResourceId id);
    const Entry* findLocked(tap::ResourceId id) const;
    void setupForRtxFromLocked(Entry& dst, Entry& src);
    void flushSubresource0Locked(Entry& e);
    void setImageHashLocked(Entry& e, hash::Hash64 image, HashOrigin origin, tap::ResourceId from, bool obsolete);
    void clearHashLocked(Entry& e);
    void syncRegistryLocked(const Entry& e);
    void maybeReleaseShadowLocked(Entry& e);
    bool shadowWanted(const Entry& e) const;
    void flushSlotLocked(tap::ResourceId id);
    void refreshBackBufferDescriptorLocked(const tap::DeviceEvent& e);

    TextureTrackerConfig m_config;
    bool m_retainShadow = false;
    std::atomic<std::uint32_t>* m_rtCounter = nullptr;
    ExternalImageRegistry* m_registry = nullptr;

    mutable std::mutex m_mutex;
    std::map<tap::ResourceId, std::unique_ptr<Entry>> m_textures;
    TextureTrackerStats m_stats;
};

} // namespace fuse::relight::capture::texture
