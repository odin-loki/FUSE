#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// G-buffer attachment slots (B5.2 — P5 §5.2).
enum class GBufferAttachment : u8 {
    NormalAo = 0,                 // RT0: RGBA16F — oct-encoded normal (xy), AO (w)
    AlbedoAlpha = 1,              // RT1: RGBA8 — albedo (rgb), alpha (a)
    RoughMetalEmissiveShading = 2, // RT2: RGBA8 — roughness, metallic, emissive mask, shading model
    Velocity = 3,                 // RT3: RG16F — motion vectors
    Depth = 4,                    // RT4: R32F — reversed-Z depth
    Emissive = 5,                 // RT5: RGBA16F — emissive colour
    Count = 6,
};

/// Per-attachment format table for the revised deferred layout.
struct GBufferLayout {
    static GpuFormat format(GBufferAttachment attachment);
    static const char* debugName(GBufferAttachment attachment);
    static u32 attachmentCount() { return static_cast<u32>(GBufferAttachment::Count); }
};

/// CPU-side octahedral normal encoding — mirrors shaders/common/gbuffer.glsl.
struct GBufferEncoding {
    static fuse::math::Vec2 encodeNormal(const fuse::math::Vec3& normal);
    static fuse::math::Vec3 decodeNormal(const fuse::math::Vec2& encoded);
    static f32 angularErrorRadians(const fuse::math::Vec3& a, const fuse::math::Vec3& b);
};

struct GBufferPackedData {
    fuse::math::Vec3 normal{};
    fuse::math::Vec3 albedo{1.f, 1.f, 1.f};
    f32 roughness = 0.5f;
    f32 metallic = 0.f;
    fuse::math::Vec3 emissive{};
    f32 ao = 1.f;
    f32 velocityX = 0.f;
    f32 velocityY = 0.f;
    u8 shadingModel = 0;
};

struct GBufferTargets {
    TextureHandle attachments[static_cast<usize>(GBufferAttachment::Count)]{};
};

struct GBufferDesc {
    u32 width = 1;
    u32 height = 1;
    bool reversedZ = true;
};

/// G-buffer target allocation scaffold — creates GPU textures via ResourceManager.
class GBuffer {
public:
    GBuffer() = default;

    bool init(ResourceManager& resources, const GBufferDesc& desc);
    void resize(u32 width, u32 height);
    void destroy();

    bool isReady() const { return m_ready; }
    const GBufferDesc& desc() const { return m_desc; }
    const GBufferTargets& targets() const { return m_targets; }

private:
    void releaseTargets();

    ResourceManager* m_resources = nullptr;
    GBufferDesc m_desc{};
    GBufferTargets m_targets{};
    bool m_ready = false;
};

} // namespace fuse::renderer
