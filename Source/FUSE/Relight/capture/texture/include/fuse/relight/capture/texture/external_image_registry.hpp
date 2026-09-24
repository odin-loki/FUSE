// FUSE Relight RL-1.4: registry of the game's (DXVK-owned) images, before bindless
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.3).
//
// Game textures stay DXVK images. Relight refers to them through this registry as external,
// non-owned resources: a slot per live image, addressed by a {index, generation} handle. Releasing a
// slot (DXVK destroyed the image: tap onImageDestroy) bumps its generation, so a stale handle never
// resolves to the image that reuses the slot. When the renderer's bindless registry takes these
// images over (external non-owned descriptors), the release listener is where it drops them; the
// handle layout is the same {index, generation} pair it validates.
//
// Thread-safe: the tap thread registers and releases, render / capture threads look up.
#pragma once

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fuse::relight::capture::texture {

struct ExternalImageHandle {
    std::uint32_t index = 0;      ///< 0 = invalid
    std::uint32_t generation = 0; ///< starts at 1 for a slot's first image

    constexpr bool valid() const { return index != 0; }
    friend constexpr bool operator==(const ExternalImageHandle&, const ExternalImageHandle&) = default;
};

/// What the registry knows about an external image.
struct ExternalImageInfo {
    tap::ResourceId texture = tap::kNoResource;
    std::uint64_t vkImage = 0;        ///< raw VkImage (DXVK-owned; never destroyed by FUSE)
    std::uint32_t type = 0;           ///< D3DRESOURCETYPE
    std::uint32_t format = 0;         ///< D3DFORMAT
    std::uint32_t width = 0, height = 0, depth = 0;
    std::uint32_t mipLevels = 0, arraySize = 0;
    std::uint32_t usage = 0, pool = 0;
    hash::Hash64 imageHash = hash::kEmptyHash;      ///< "remix.tex" (0 until hashed)
    hash::Hash64 descriptorHash = hash::kEmptyHash; ///< "remix.rtdesc" (render targets)
};

class ExternalImageRegistry {
public:
    /// Called (outside the registry lock) with the released handle and the slot's last info.
    using ReleaseListener = std::function<void(ExternalImageHandle, const ExternalImageInfo&)>;

    /// Registers `info` (info.vkImage must be non-zero) for info.texture. A texture registered
    /// again with the same VkImage keeps its handle (the info is refreshed); with a different one
    /// the old slot is released first. Returns an invalid handle when vkImage is 0.
    ExternalImageHandle registerImage(const ExternalImageInfo& info);
    /// Releases the texture's slot (generation bump). False when it was not registered.
    bool release(tap::ResourceId texture);
    /// Releases every slot (device destruction).
    void releaseAll();

    /// Updates the hashes of a registered texture. False when it is not registered.
    bool updateHashes(tap::ResourceId texture, hash::Hash64 imageHash, hash::Hash64 descriptorHash);

    /// The live info behind `handle`; nullopt for a stale or invalid handle.
    std::optional<ExternalImageInfo> lookup(ExternalImageHandle handle) const;
    /// The handle of a registered texture (invalid when none).
    ExternalImageHandle handleOf(tap::ResourceId texture) const;
    /// Handles of live images with this image hash (several textures can share content).
    std::vector<ExternalImageHandle> findByImageHash(hash::Hash64 imageHash) const;

    void setReleaseListener(ReleaseListener listener);

    std::size_t liveCount() const;
    /// Slots ever allocated (live + free).
    std::size_t capacity() const;

private:
    struct Slot {
        std::uint32_t generation = 0;
        bool live = false;
        ExternalImageInfo info;
    };

    // Expects m_mutex held; returns the released slot's info.
    ExternalImageInfo releaseSlotLocked(std::uint32_t index);

    mutable std::mutex m_mutex;
    std::vector<Slot> m_slots; ///< m_slots[i] is handle index i + 1
    std::vector<std::uint32_t> m_free;
    std::unordered_map<tap::ResourceId, std::uint32_t> m_byTexture;
    std::size_t m_live = 0;
    ReleaseListener m_onRelease;
};

} // namespace fuse::relight::capture::texture
