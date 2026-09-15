#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// 3D probe grid dimensions (B5.6 — P5 §5.6).
struct DDGIGridDims {
    u32 x = 16;
    u32 y = 8;
    u32 z = 16;
};

/// DDGI probe volume description — default 16×8×16 = 2048 probes.
struct DDGIDesc {
    fuse::math::Vec3 grid_origin{};
    fuse::math::Vec3 probe_spacing{2.f, 2.f, 2.f};
    DDGIGridDims grid_dims{};
    u32 rays_per_probe = 256;
    u32 probes_per_frame = 64;
    u32 irradiance_res = 8;
    u32 depth_res = 16;
    f32 hysteresis = 0.97f;
    f32 max_ray_distance = 20.f;
};

/// GPU irradiance cache handles for the probe volume.
struct ProbeVolume {
    TextureHandle irradiance_atlas{};
    TextureHandle depth_atlas{};
    BufferHandle probe_offsets{};
    u32 probe_count = 0;
};

/// Per-probe irradiance cache entry — CPU-side hysteresis scaffold for tests.
struct IrradianceCacheEntry {
    fuse::math::Vec3 irradiance{};
    f32 mean_depth = 0.f;
    f32 depth_variance = 0.f;
};

/// Combined probe data exposed to deferred shading kernels.
struct ProbeData {
    TextureHandle irradiance_atlas{};
    TextureHandle depth_atlas{};
    BufferHandle probe_offsets{};
    DDGIDesc desc{};
};

enum class DdgiBackend : u8 {
    Stub,
    CpuReference,
    Cuda,
};

struct DdgiInfo {
    bool valid = false;
    DdgiBackend backend = DdgiBackend::Stub;
    u32 probe_count = 0;
    const char* message = nullptr;
};

struct DDGIUpdateStats {
    u32 probes_scheduled = 0;
    u32 frame_index = 0;
    bool kernel_launched = false;
};

struct DDGISampleRequest {
    fuse::math::Vec3 world_position{};
    fuse::math::Vec3 world_normal{};
};

struct DDGISampleResult {
    fuse::math::Vec3 irradiance{};
    u32 nearest_probe = UINT32_MAX;
    bool valid = false;
};

/// Integer probe coordinate within the 3D grid (B5.6 deepen).
struct ProbeGridCoord {
    u32 x = 0;
    u32 y = 0;
    u32 z = 0;
};

/// Per-probe validity flags for border/interior classification (B5.6 deepen).
struct ProbeValidityFlags {
    bool valid = false;
    bool is_border = false;
    bool interior = false;
    /// True when the probe has a full 2×2×2 neighbourhood for trilinear sampling.
    bool has_trilinear_neighbourhood = false;
};

/// CPU-side octahedral direction encoding for probe irradiance atlas tiles (B5.6 deepen).
/// Mirrors `GBufferEncoding` and the deferred-shade probe sampling path.
struct DdgiIrradianceEncoding {
    static fuse::math::Vec2 encodeDirection(const fuse::math::Vec3& direction);
    static fuse::math::Vec3 decodeDirection(const fuse::math::Vec2& encoded);
    /// Unit-square UV within a probe's octahedral tile.
    static fuse::math::Vec2 directionToAtlasUV(const fuse::math::Vec3& direction);
    /// Texel offset within a probe tile — clamped to [0, irradiance_res - 1].
    static fuse::math::Vec2 directionToTexelOffset(const fuse::math::Vec3& direction, u32 irradiance_res);
    static f32 angularErrorRadians(const fuse::math::Vec3& a, const fuse::math::Vec3& b);
};

/// Probe grid indexing + atlas layout helpers — mirrors deferred-shade probe sampling.
struct ProbeGridLayout {
    static ProbeGridCoord probeCoordFromIndex(const DDGIDesc& desc, u32 probe_index);
    static u32 probeIndexFromCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    static bool isValidProbeCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    static bool isValidProbeIndex(const DDGIDesc& desc, u32 probe_index);
    static bool isBorderProbeCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    static ProbeValidityFlags probeValidity(const DDGIDesc& desc, const ProbeGridCoord& coord);
    static ProbeValidityFlags probeValidityFromIndex(const DDGIDesc& desc, u32 probe_index);
    /// Fractional grid coordinates — origin cell centre is (0,0,0).
    static fuse::math::Vec3 worldToProbeGridCoord(const DDGIDesc& desc,
                                                  const fuse::math::Vec3& world_position);
    /// Clamp fractional grid coordinates to the valid probe index range.
    static fuse::math::Vec3 clampWorldToProbeGridCoord(const DDGIDesc& desc,
                                                       const fuse::math::Vec3& grid_coord);
    static ProbeGridCoord clampProbeGridCoord(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// Top-left texel of the probe's octahedral irradiance tile in the atlas.
    static fuse::math::Vec2 probeIrradianceAtlasOrigin(const DDGIDesc& desc, const ProbeGridCoord& coord);
    /// Absolute atlas texel for a world-space direction sample within a probe tile.
    static fuse::math::Vec2 probeIrradianceAtlasTexel(const DDGIDesc& desc,
                                                      const ProbeGridCoord& coord,
                                                      const fuse::math::Vec3& direction);
    /// Top-left texel of the probe's depth-variance tile in the atlas.
    static fuse::math::Vec2 probeDepthAtlasOrigin(const DDGIDesc& desc, const ProbeGridCoord& coord);
};

/// CPU-side probe grid helpers — mirrors CUDA scheduling without GPU.
namespace ddgi_util {
u32 probeCount(const DDGIDesc& desc);
fuse::math::Vec3 probeWorldPosition(const DDGIDesc& desc, u32 probe_index);
u32 irradianceAtlasWidth(const DDGIDesc& desc);
u32 irradianceAtlasHeight(const DDGIDesc& desc);
u32 depthAtlasWidth(const DDGIDesc& desc);
u32 depthAtlasHeight(const DDGIDesc& desc);
void scheduleProbeUpdates(u32 frame_index,
                          u32 probe_count,
                          u32 probes_per_frame,
                          u32* out_indices,
                          u32 max_indices,
                          u32* out_count);
fuse::math::Vec3 blendIrradiance(const fuse::math::Vec3& previous,
                                 const fuse::math::Vec3& incoming,
                                 f32 hysteresis);
fuse::math::Vec3 lerpIrradiance(const fuse::math::Vec3& a, const fuse::math::Vec3& b, f32 t);
/// Bilinear irradiance lerp within a probe's octahedral tile (CPU stub).
/// `samples` must point to four irradiance values in [00, 10, 01, 11] order.
fuse::math::Vec3 bilinearTileIrradiance(const fuse::math::Vec3* samples, f32 u, f32 v);
fuse::math::Vec3 trilinearProbeIrradiance(const DDGIDesc& desc,
                                          const fuse::math::Vec3& world_position,
                                          const IrradianceCacheEntry* cache,
                                          u32 cache_count);
u32 nearestProbeIndex(const DDGIDesc& desc, const fuse::math::Vec3& world_position);
} // namespace ddgi_util

DdgiInfo ddgi_info();

/// DDGI probe volume scaffold — allocates atlas textures via ResourceManager.
class DDGI {
public:
    DDGI() = default;

    bool init(const DDGIDesc& desc, ResourceManager& resources);
    void destroy();

    bool isReady() const { return m_ready; }
    const DDGIDesc& desc() const { return m_desc; }
    const ProbeData& data() const { return m_data; }
    const ProbeVolume& volume() const { return m_volume; }
    const IrradianceCacheEntry& cacheEntry(u32 probe_index) const;
    const DDGIUpdateStats& lastUpdateStats() const { return m_last_update; }
    const DdgiInfo& info() const { return m_info; }

    /// Update a rotating subset of probes — CUDA path when available, CPU reference otherwise.
    bool update(u32 frame_index, void* cuda_stream = nullptr);

    /// Sample nearest-probe irradiance (CPU stub for deferred shading integration).
    DDGISampleResult sampleIrradiance(const DDGISampleRequest& request) const;

private:
    void releaseResources();
    bool allocateResources(ResourceManager& resources);

    DDGIDesc m_desc{};
    ProbeData m_data{};
    ProbeVolume m_volume{};
    std::vector<IrradianceCacheEntry> m_cache;
    DDGIUpdateStats m_last_update{};
    DdgiInfo m_info{};
    ResourceManager* m_resources = nullptr;
    bool m_ready = false;
};

/// Host launcher for probe trace + blend kernels — stub until CUDA kernels land.
bool launch_ddgi_probe_update(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              void* cuda_stream = nullptr);

} // namespace fuse::renderer
