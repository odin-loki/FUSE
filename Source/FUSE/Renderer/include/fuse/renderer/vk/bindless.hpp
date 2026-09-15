#pragma once

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

static constexpr u32 kMaxTextures = 65536;
static constexpr u32 kMaxBuffers = 65536;
static constexpr u32 kMaxSamplers = 1024;

/// Global descriptor table scaffolding — stub indices until B2.4 pipeline wiring.
class BindlessDescriptors {
public:
    void init(const VulkanDevice& device);
    void destroy(const VulkanDevice& device);

    u32 registerTexture(const Texture& texture, bool storage = false);
    u32 registerBuffer(const Buffer& buffer);
    u32 registerSampler(void* samplerHandle);
    void unregisterTexture(u32 index);
    void unregisterBuffer(u32 index);
    void unregisterSampler(u32 index);

    void* layoutHandle() const { return m_layout; }
    void* descriptorSetHandle() const { return m_set; }

    u32 registeredTextureCount() const { return m_textureSlots.size(); }
    u32 registeredBufferCount() const { return m_bufferSlots.size(); }
    u32 registeredSamplerCount() const { return m_samplerSlots.size(); }

private:
    void* m_pool = nullptr;
    void* m_layout = nullptr;
    void* m_set = nullptr;

    std::vector<u32> m_textureSlots;
    std::vector<u32> m_bufferSlots;
    std::vector<u32> m_samplerSlots;
    std::vector<u32> m_freeTextureIndices;
    std::vector<u32> m_freeBufferIndices;
    std::vector<u32> m_freeSamplerIndices;
    bool m_initialized = false;
};

} // namespace fuse::renderer
