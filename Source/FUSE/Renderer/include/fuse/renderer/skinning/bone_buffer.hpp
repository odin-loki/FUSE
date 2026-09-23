#pragma once

#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

struct BoneBufferDesc {
    /// Capacity per frame slot, in 4x4 f32 matrices (64 bytes each).
    u32 maxBones = 256;
    const char* name = "fuse.skinning.bone_palette";
};

struct BoneBufferStats {
    u64 uploads = 0;
    u64 bytesUploaded = 0;
    /// GPU buffers created over the lifetime (kFramesInFlight after init; never grows per frame).
    u32 buffersCreated = 0;
    u32 rejectedUploads = 0;
};

/// GPU skinning bone palette (B7 Animator -> renderer): one host-visible storage buffer per frame
/// slot, created once. `upload` writes the Animator's `bone_palette` (world * inverse bind per
/// bone) into the slot the frame is recording, so the GPU never reads a palette the CPU is
/// overwriting while an older frame is still in flight. No per-frame allocation.
class BoneBuffer {
public:
    static constexpr u32 kSlots = kFramesInFlight;

    BoneBuffer() = default;
    ~BoneBuffer();

    BoneBuffer(const BoneBuffer&) = delete;
    BoneBuffer& operator=(const BoneBuffer&) = delete;

    bool init(ResourceManager& resources, const BoneBufferDesc& desc = {});
    void destroy();

    bool isReady() const { return m_resources != nullptr; }
    u32 maxBones() const { return m_desc.maxBones; }

    /// Copy `boneCount` column-major 4x4 f32 matrices into frame slot `frameSlot % kSlots`.
    /// Returns false when not ready, `matrices` is null or `boneCount` exceeds `maxBones`.
    bool upload(u32 frameSlot, const void* matrices, u32 boneCount);

    BufferHandle buffer(u32 frameSlot) const { return m_buffers[frameSlot % kSlots]; }
    u32 boneCount(u32 frameSlot) const { return m_boneCounts[frameSlot % kSlots]; }
    const BoneBufferStats& stats() const { return m_stats; }

private:
    ResourceManager* m_resources = nullptr;
    BoneBufferDesc m_desc{};
    BufferHandle m_buffers[kSlots]{};
    u32 m_boneCounts[kSlots] = {};
    BoneBufferStats m_stats{};
};

} // namespace fuse::renderer
