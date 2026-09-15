#pragma once

#include <fuse/terrain/terrain_desc.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::terrain {

/// CPU heightmap storage with bilinear sampling (B7.5 stub).
class Heightfield {
public:
    void init(const TerrainDesc& desc);
    void destroy();

    void resize(u32 resolution);
    void fill(f32 height);
    void set_height(u32 x, u32 z, f32 height);
    [[nodiscard]] f32 get_height_texel(u32 x, u32 z) const;

    /// Bilinear sample in world XZ; returns metres.
    [[nodiscard]] f32 sample_height(f32 world_x, f32 world_z) const;
    [[nodiscard]] vec3 sample_normal(f32 world_x, f32 world_z) const;

    [[nodiscard]] const TerrainDesc& desc() const { return m_desc; }
    [[nodiscard]] u32 resolution() const { return m_desc.resolution; }
    [[nodiscard]] bool is_initialized() const { return m_initialized; }
    [[nodiscard]] const std::vector<f32>& heights() const { return m_heights; }

private:
    [[nodiscard]] f32 sample_bilinear(f32 u, f32 v) const;
    [[nodiscard]] bool in_bounds(u32 x, u32 z) const;

    TerrainDesc m_desc{};
    std::vector<f32> m_heights;
    bool m_initialized = false;
};

} // namespace fuse::terrain
