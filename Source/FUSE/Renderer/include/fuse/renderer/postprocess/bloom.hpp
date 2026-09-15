#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Bloom pass parameters (B5.10 — P5 §5.10).
struct BloomParams {
    f32 threshold = 1.f;
    f32 knee = 0.5f;
    f32 intensity = 0.05f;
    u32 mip_levels = 7;
    f32 scatter = 0.7f;
};

/// CPU-side bloom threshold + contribution stub (CUDA pyramid deferred).
class Bloom {
public:
    void setParams(const BloomParams& params) { m_params = params; }
    const BloomParams& params() const { return m_params; }
    bool isReady() const { return m_ready; }
    void init();
    void destroy();

    static f32 luminance(const fuse::math::Vec3& rgb);
    static fuse::math::Vec3 extractBright(const fuse::math::Vec3& hdr, const BloomParams& params);
    static fuse::math::Vec3 apply(const fuse::math::Vec3& hdr, const BloomParams& params);

private:
    BloomParams m_params{};
    bool m_ready = false;
};

} // namespace fuse::renderer
