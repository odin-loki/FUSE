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
    static u32 channelCount(GBufferAttachment attachment);
    static bool validateAttachmentFormats();
};

/// CPU-side octahedral normal encoding — mirrors shaders/common/gbuffer.glsl.
///
/// `encodeNormal` / `decodeNormal` are the unsigned [0,1]^2 form (`encode_normal` / `decode_normal`).
/// RT0 is a float format, so the G-buffer stores the signed [-1,1]^2 form instead: half floats carry
/// twice the precision there, and `encodeNormalRgba16f` additionally picks the half-representable
/// neighbour with the smallest angular error. Only that combination keeps the worst-case round trip
/// through RGBA16F below 0.001 rad (plain unsigned storage reaches ~0.002 rad near the octant seams).
struct GBufferEncoding {
    static fuse::math::Vec2 encodeNormal(const fuse::math::Vec3& normal);
    static fuse::math::Vec3 decodeNormal(const fuse::math::Vec2& encoded);

    static fuse::math::Vec2 encodeOctSigned(const fuse::math::Vec3& normal);
    static fuse::math::Vec3 decodeOctSigned(const fuse::math::Vec2& oct);

    /// Signed oct encoding whose components are exactly representable as IEEE half floats.
    static fuse::math::Vec2 encodeNormalRgba16f(const fuse::math::Vec3& normal);

    static f32 angularErrorRadians(const fuse::math::Vec3& a, const fuse::math::Vec3& b);
};

/// Storage-format quantisation of MRT channels (what the attachment actually keeps).
struct GBufferQuantize {
    /// IEEE 754 binary16 conversion, round-to-nearest-even; overflow -> inf, NaN preserved.
    static u16 floatToHalf(f32 value);
    static f32 halfToFloat(u16 bits);
    static f32 toHalf(f32 value) { return halfToFloat(floatToHalf(value)); }
    /// Next representable half above / below `bits` (signed zero treated as zero).
    static u16 halfNextUp(u16 bits);
    static u16 halfNextDown(u16 bits);
    /// UNORM8 round trip (clamped to [0,1], round to nearest).
    static f32 toUnorm8(f32 value);
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

/// CPU MRT channel bundle — mirrors `write_gbuffer` outputs in shaders/common/gbuffer.glsl.
struct GBufferMrt {
    fuse::math::Vec4 rt0{}; // xy = signed oct normal (half-exact), w = AO
    fuse::math::Vec4 rt1{}; // rgb = albedo, a = opacity
    fuse::math::Vec4 rt2{}; // r = roughness, g = metallic, b = emissive mask, a = shading model / 255
    fuse::math::Vec4 rt3{}; // xy = velocity
    fuse::math::Vec4 rt5{}; // rgb = emissive radiance
};

/// CPU pack/unpack helpers — kept in sync with gbuffer.glsl for layout validation tests.
struct GBufferPacking {
    static GBufferMrt pack(const GBufferPackedData& data, f32 alpha = 1.f);
    static GBufferPackedData unpack(const GBufferMrt& mrt);
    /// Applies each attachment's storage format (RGBA16F / RGBA8 / RG16F) to the channel values.
    static GBufferMrt quantizeToStorage(const GBufferMrt& mrt);
};

/// Largest finite RGBA16F value — emissive radiance is clamped here so RT5 never stores inf.
inline constexpr f32 kGBufferMaxHalf = 65504.f;

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
